"""
Krita MCP Bridge - HTTP server for external paint commands in Krita
Allows Claude (or any MCP client) to paint by sending commands to this plugin.
"""

import hmac
import json
import os
import re
import secrets
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs, urlparse

from krita import *
from PyQt5.QtCore import QPointF, QRectF, QThread, QTimer, pyqtSignal
from PyQt5.QtGui import QColor
from PyQt5.QtWidgets import QApplication, QMessageBox, QWidget

# Configuration - customize these as needed
SERVER_PORT = 5678

# --- Solstice hardening: bearer token for the loopback bridge ----------
# The token is generated once and stored in the application settings
# ("kritamcp" group). External MCP servers (krita-cli, server.py) must
# send it as `Authorization: Bearer <token>`. /health stays open so the
# server can probe liveness before it has the token.
#
# kritarc is only flushed when the app saves settings (typically on exit),
# so a freshly generated token is mirrored to <config>/kritamcp.token
# immediately — clients can authenticate in the same session it was created.
def _token_sidecar_path():
    from PyQt5.QtCore import QStandardPaths
    cfg = QStandardPaths.writableLocation(QStandardPaths.AppConfigLocation)
    return os.path.join(cfg, "kritamcp.token")


def _bridge_token():
    app = Krita.instance()
    token = app.readSetting("kritamcp", "token", "")
    if not token:
        try:
            with open(_token_sidecar_path(), "r", encoding="ascii") as f:
                token = f.read().strip()
        except OSError:
            pass
    if not token:
        token = secrets.token_hex(24)
        app.writeSetting("kritamcp", "token", token)
    try:
        with open(_token_sidecar_path(), "w", encoding="ascii") as f:
            f.write(token)
        try:
            os.chmod(_token_sidecar_path(), 0o600)
        except OSError:
            pass
    except OSError:
        pass
    return token

BRIDGE_TOKEN = None  # resolved lazily on first request (Krita must exist)
BRIDGE_LISTENING = False
CANVAS_OUTPUT_DIR = os.path.expanduser("~/krita-mcp-output")
TEMPLATE_DIR = os.path.join(os.environ.get("APPDATA", ""), "krita", "templates")

SUPPORTED_ACTIONS = [
    # Painting
    "new_canvas", "set_color", "set_brush", "stroke",
    "fill", "draw_shape", "get_canvas", "undo", "redo",
    "trigger_action", "screenshot", "clear", "save",
    "get_color_at", "list_brushes", "open_file",
    # Layer management
    "add_layer", "remove_layer", "list_layers",
    "set_active_layer", "set_layer_opacity",
    "set_layer_blend_mode", "set_layer_visibility",
    "move_layer", "duplicate_layer", "merge_down",
    # Templates
    "create_from_template", "save_as_template",
    "list_templates", "get_layer_presets",
    # Document
    "get_document_info", "resize_canvas",
    "scale_image", "set_resolution", "flatten_image",
    # Selection
    "select_all", "deselect", "select_rect",
    # Filters
    "apply_filter", "list_filters",
    # Export
    "export_layers", "export_document",
]


def _resolve_allowed_path(path, *, for_write=True):
    """Validate and resolve a path against allowed roots.

    Returns (resolved_path, None) on success, or (None, error_dict) on rejection.
    """
    if not path or not isinstance(path, str):
        return None, {"error": "No path specified"}

    # Reject '..' segments in the raw string
    raw_parts = path.replace("\\", "/").split("/")
    if ".." in raw_parts:
        return None, {"error": "Invalid path: path traversal ('..') not allowed"}

    # Reject UNC and device namespace paths (\\host\share, //host/share, \\?\, \\.\)
    norm_path = path.replace("/", "\\")
    if norm_path.startswith("\\\\") or path.startswith(("//", "\\\\")) or "\\\\?\\" in norm_path or "\\\\.\\" in norm_path:
        return None, {"error": "Invalid path: UNC and device namespace paths are not allowed"}

    # Build allowed roots list
    allowed_roots = []

    # 1. CANVAS_OUTPUT_DIR
    if CANVAS_OUTPUT_DIR:
        try:
            allowed_roots.append(os.path.realpath(CANVAS_OUTPUT_DIR))
        except Exception:
            pass

    # 2. TEMPLATE_DIR
    if TEMPLATE_DIR:
        try:
            allowed_roots.append(os.path.realpath(TEMPLATE_DIR))
        except Exception:
            pass

    # 3. Active document folder if any
    try:
        app = Krita.instance()
        doc = app.activeDocument() if app else None
        if doc and doc.fileName():
            doc_dir = os.path.dirname(os.path.realpath(doc.fileName()))
            if doc_dir:
                allowed_roots.append(doc_dir)
    except Exception:
        pass

    # 4. User home Documents and Pictures
    home = os.path.expanduser("~")
    for subdir in ("Documents", "Pictures"):
        try:
            allowed_roots.append(os.path.realpath(os.path.join(home, subdir)))
        except Exception:
            pass

    try:
        from PyQt5.QtCore import QStandardPaths
        for loc in (
            QStandardPaths.StandardLocation.DocumentsLocation,
            QStandardPaths.StandardLocation.PicturesLocation,
        ):
            sp = QStandardPaths.writableLocation(loc)
            if sp:
                allowed_roots.append(os.path.realpath(sp))
    except Exception:
        pass

    # 5. Temp directories
    temp_dirs = [tempfile.gettempdir()]
    for env_k in ("TEMP", "TMP", "TMPDIR"):
        val = os.environ.get(env_k)
        if val:
            temp_dirs.append(val)
    for td in temp_dirs:
        try:
            allowed_roots.append(os.path.realpath(td))
        except Exception:
            pass

    # 6. Extra roots from KRITAMCP_ALLOWED_ROOTS environment variable
    env_roots = os.environ.get("KRITAMCP_ALLOWED_ROOTS", "")
    if env_roots:
        for r in env_roots.split(os.pathsep):
            if r.strip():
                try:
                    allowed_roots.append(os.path.realpath(r.strip()))
                except Exception:
                    pass

    # 7. Roots from kritarc setting
    try:
        app = Krita.instance()
        if app:
            rc_roots = app.readSetting("kritamcp", "allowed_roots", "")
            if rc_roots:
                for r in rc_roots.split(os.pathsep):
                    if r.strip():
                        try:
                            allowed_roots.append(os.path.realpath(r.strip()))
                        except Exception:
                            pass
    except Exception:
        pass

    try:
        resolved = os.path.realpath(path)
    except Exception as e:
        return None, {"error": f"Invalid path resolution: {e}"}

    target_comp = resolved.lower() if os.name == 'nt' else resolved

    is_allowed = False
    for root in allowed_roots:
        root_comp = root.lower() if os.name == 'nt' else root
        try:
            if os.path.commonpath([target_comp, root_comp]) == root_comp:
                is_allowed = True
                break
        except (ValueError, OSError):
            continue

    if not is_allowed:
        return None, {"error": f"Path is not in an allowed directory: {path}"}

    return resolved, None

# Predefined layer stack presets for template creation
LAYER_PRESETS = {
    "comic": [
        {"name": "Background", "type": "paintlayer", "blend": "normal", "opacity": 255},
        {"name": "Sketch", "type": "paintlayer", "blend": "normal", "opacity": 200},
        {"name": "Inks", "type": "paintlayer", "blend": "normal", "opacity": 255},
        {"name": "Flat Colors", "type": "paintlayer", "blend": "normal", "opacity": 255},
        {"name": "Shading", "type": "paintlayer", "blend": "multiply", "opacity": 200},
        {"name": "Highlights", "type": "paintlayer", "blend": "screen", "opacity": 150},
        {"name": "Text", "type": "paintlayer", "blend": "normal", "opacity": 255},
    ],
    "concept_art": [
        {"name": "Background", "type": "paintlayer", "blend": "normal", "opacity": 255},
        {"name": "Rough Sketch", "type": "paintlayer", "blend": "normal", "opacity": 150},
        {"name": "Clean Sketch", "type": "paintlayer", "blend": "normal", "opacity": 200},
        {"name": "Base Colors", "type": "paintlayer", "blend": "normal", "opacity": 255},
        {"name": "Shadows", "type": "paintlayer", "blend": "multiply", "opacity": 180},
        {"name": "Rim Light", "type": "paintlayer", "blend": "screen", "opacity": 120},
        {"name": "Details", "type": "paintlayer", "blend": "normal", "opacity": 255},
        {"name": "Effects", "type": "paintlayer", "blend": "overlay", "opacity": 200},
    ],
    "animation": [
        {"name": "Background", "type": "paintlayer", "blend": "normal", "opacity": 255},
        {"name": "Reference", "type": "paintlayer", "blend": "normal", "opacity": 100},
        {"name": "Rough", "type": "paintlayer", "blend": "normal", "opacity": 200},
        {"name": "Clean", "type": "paintlayer", "blend": "normal", "opacity": 255},
        {"name": "Color", "type": "paintlayer", "blend": "normal", "opacity": 255},
    ],
    "illustration": [
        {"name": "Background", "type": "paintlayer", "blend": "normal", "opacity": 255},
        {"name": "Sketch", "type": "paintlayer", "blend": "normal", "opacity": 180},
        {"name": "Lineart", "type": "paintlayer", "blend": "normal", "opacity": 255},
        {"name": "Base Colors", "type": "paintlayer", "blend": "normal", "opacity": 255},
        {"name": "Shading", "type": "paintlayer", "blend": "multiply", "opacity": 200},
        {"name": "Highlights", "type": "paintlayer", "blend": "screen", "opacity": 150},
        {"name": "Overlay Effects", "type": "paintlayer", "blend": "overlay", "opacity": 180},
    ],
    "painting": [
        {"name": "Canvas", "type": "paintlayer", "blend": "normal", "opacity": 255},
        {"name": "Underpainting", "type": "paintlayer", "blend": "normal", "opacity": 255},
        {"name": "Main", "type": "paintlayer", "blend": "normal", "opacity": 255},
        {"name": "Details", "type": "paintlayer", "blend": "normal", "opacity": 255},
        {"name": "Glaze", "type": "paintlayer", "blend": "overlay", "opacity": 100},
    ],
    "minimal": [
        {"name": "Background", "type": "paintlayer", "blend": "normal", "opacity": 255},
        {"name": "Layer 1", "type": "paintlayer", "blend": "normal", "opacity": 255},
    ],
}

class CommandQueue:
    """Thread-safe command queue for passing commands from HTTP thread to main thread."""
    def __init__(self):
        self.queue = []
        self.results = {}
        self.lock = threading.Lock()
        self.result_event = threading.Event()

    def push(self, command_id, command):
        with self.lock:
            self.queue.append((command_id, command))

    def pop(self):
        with self.lock:
            if self.queue:
                return self.queue.pop(0)
            return None

    def set_result(self, command_id, result):
        with self.lock:
            self.results[command_id] = result
        self.result_event.set()

    def get_result(self, command_id, timeout=120):
        """Wait for result with timeout.

        The default timeout of 120s is important — canvas export and save
        operations can take a long time on large canvases. The original 30s
        default caused frequent timeouts. The MCP server's send_command()
        timeout must match or exceed this value.
        """
        start = threading.Event()
        for _ in range(int(timeout * 10)):  # Check every 100ms
            with self.lock:
                if command_id in self.results:
                    result = self.results.pop(command_id)
                    return result
            self.result_event.wait(0.1)
            self.result_event.clear()
        return {"error": "Timeout waiting for command execution"}

# Global command queue
command_queue = CommandQueue()
command_counter = 0

class PaintRequestHandler(BaseHTTPRequestHandler):
    """HTTP request handler for paint commands."""

    def log_message(self, format, *args):
        # Suppress HTTP logging
        pass

    def send_json_response(self, data, status=200):
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.end_headers()
        self.wfile.write(json.dumps(data).encode())

    def _check_host(self):
        """Host header check: allow only 127.0.0.1[:port], localhost[:port], [::1][:port]."""
        host_hdr = self.headers.get('Host', '').strip()
        host = host_hdr
        if host.startswith('['):
            bracket_end = host.find(']')
            if bracket_end != -1:
                host_part = host[:bracket_end + 1]
            else:
                host_part = host
        else:
            host_part = host.split(':')[0]

        allowed = ('127.0.0.1', 'localhost', '[::1]')
        if host_part not in allowed:
            self.send_json_response({"error": "Forbidden: invalid Host header"}, 403)
            return False
        return True

    def _authorized(self):
        """Bearer-token check. Returns True if the request may proceed."""
        global BRIDGE_TOKEN
        if BRIDGE_TOKEN is None:
            BRIDGE_TOKEN = _bridge_token()
        header = self.headers.get('Authorization', '')
        provided = header[7:] if header.startswith('Bearer ') else ''
        if provided and hmac.compare_digest(provided, BRIDGE_TOKEN):
            return True
        self.send_json_response({"error": "Unauthorized"}, 401)
        return False

    def do_GET(self):
        """Handle GET requests - mainly for health check."""
        if not self._check_host():
            return

        parsed = urlparse(self.path)

        if parsed.path == '/health':
            resp = {"status": "ok", "plugin": "kritamcp"}
            if BRIDGE_LISTENING:
                resp["bridge"] = "listening"
            self.send_json_response(resp)
        elif parsed.path == '/info':
            if not self._authorized():
                return
            self.send_json_response({
                "status": "ok",
                "canvas_dir": CANVAS_OUTPUT_DIR,
                "template_dir": TEMPLATE_DIR,
                "commands": list(SUPPORTED_ACTIONS),
            })
        else:
            self.send_json_response({"error": "Unknown endpoint"}, 404)

    def do_POST(self):
        """Handle POST requests - paint commands."""
        global command_counter

        if not self._check_host():
            return

        if not self._authorized():
            return

        cl_header = self.headers.get('Content-Length')
        if cl_header is None:
            self.send_json_response({"error": "Missing Content-Length"}, 400)
            return

        try:
            content_length = int(cl_header)
            if content_length < 0:
                raise ValueError("Negative Content-Length")
        except (ValueError, TypeError):
            self.send_json_response({"error": "Invalid Content-Length"}, 400)
            return

        MAX_CONTENT_LENGTH = 50 * 1024 * 1024  # 50 MiB
        if content_length > MAX_CONTENT_LENGTH:
            self.send_json_response(
                {"error": "Payload Too Large: Content-Length exceeds 50 MiB"}, 413
            )
            return

        body = self.rfile.read(content_length).decode('utf-8')

        try:
            command = json.loads(body)
        except json.JSONDecodeError:
            self.send_json_response({"error": "Invalid JSON"}, 400)
            return

        # Assign command ID and queue it
        command_counter += 1
        command_id = command_counter
        command_queue.push(command_id, command)

        # Wait for result from main thread
        result = command_queue.get_result(command_id)

        if "error" in result:
            self.send_json_response(result, 500)
        else:
            self.send_json_response(result)


class ServerThread(QThread):
    """Thread to run HTTP server without blocking Krita UI."""

    def __init__(self, port):
        super().__init__()
        self.port = port
        self.server = None

    def run(self):
        global BRIDGE_LISTENING
        try:
            self.server = HTTPServer(('127.0.0.1', self.port), PaintRequestHandler)
            BRIDGE_LISTENING = True
            print(f"[KritaMCP] HTTP server started on 127.0.0.1:{self.port}", flush=True)
            self.server.serve_forever()
        except OSError as e:
            BRIDGE_LISTENING = False
            print(f"[KritaMCP] port {self.port} in use or failed to bind: {e}", file=sys.stderr, flush=True)
        finally:
            BRIDGE_LISTENING = False

    def stop(self):
        global BRIDGE_LISTENING
        BRIDGE_LISTENING = False
        if self.server:
            self.server.shutdown()


class KritaMCPExtension(Extension):
    """Main Krita extension class."""

    def __init__(self, parent):
        super().__init__(parent)
        self.server_thread = None
        self.timer = None
        self.current_brush_size = 20
        self.current_opacity = 1.0
        # Commands are only executed once the main window is fully created:
        # window.addView() deadlocks when called during window construction.
        self._window_ready = False

    def setup(self):
        """Called when extension is loaded."""
        pass

    def createActions(self, window):
        """Called when a new window is created."""
        # Ensure output directory exists
        os.makedirs(CANVAS_OUTPUT_DIR, exist_ok=True)

        # Start HTTP server
        if self.server_thread is None:
            self.server_thread = ServerThread(SERVER_PORT)
            self.server_thread.start()

        # Start timer to process command queue
        if self.timer is None:
            self.timer = QTimer()
            self.timer.timeout.connect(self.process_commands)
            self.timer.start(50)  # Check every 50ms

        # windowCreated fires when main-window construction completes
        # (via checkSanity, ~1s after the ctor). windowIsCreated exists in
        # the API but is never emitted. The Notifier must be activated or
        # its signals never fire.
        try:
            notifier = Krita.instance().notifier()
            notifier.setActive(True)
            notifier.windowCreated.connect(self._on_window_ready)
            print("[KritaMCP] windowCreated signal connected", flush=True)
        except Exception as e:
            print(f"[KritaMCP] windowCreated connect failed: {e}", flush=True)
        # Plugin enabled at runtime with the window already up: ready now.
        # Checked once, here — during initial construction isVisible() can
        # read True too early, so it must not be used as a running gate.
        try:
            qw = window.qwindow() if window else None
            vis = qw.isVisible() if qw is not None else None
            print(f"[KritaMCP] createActions window visible={vis}", flush=True)
            if vis:
                self._window_ready = True
        except Exception as e:
            print(f"[KritaMCP] visibility probe failed: {e}", flush=True)
        # Last-resort fallback: if the signal never arrives, force-ready
        # after a bounded delay rather than starving the bridge forever.
        QTimer.singleShot(15000, self._force_ready)

    def _on_window_ready(self):
        self._window_ready = True
        print("[KritaMCP] window ready — command processing enabled", flush=True)

    def _force_ready(self):
        if not self._window_ready:
            print("[KritaMCP] readiness fallback fired (signal missed)", flush=True)
            self._window_ready = True

    def process_commands(self):
        """Process commands from queue in main thread."""
        item = command_queue.pop()
        if item is None:
            return

        if not self._window_ready:
            # Window still constructing — requeue so the requester waits
            # inside its command timeout instead of deadlocking in addView.
            command_queue.push(item[0], item[1])
            return

        command_id, command = item
        print(f"[KritaMCP] exec #{command_id} {command.get('action')}", flush=True)
        result = self.execute_command(command)
        command_queue.set_result(command_id, result)
        print(f"[KritaMCP] done #{command_id} {command.get('action')}", flush=True)

    def execute_command(self, command):
        """Execute a paint command and return result."""
        try:
            action = command.get("action")
            params = command.get("params", {})

            if action not in SUPPORTED_ACTIONS:
                return {"error": f"Unknown action: {action}"}

            handler = getattr(self, f"cmd_{action}", None)
            if handler:
                return handler(params)
            return {"error": f"Unknown action: {action}"}

        except Exception as e:
            return {"error": str(e)}

    def get_active_document(self):
        """Get active document or return None."""
        app = Krita.instance()
        return app.activeDocument()

    def get_active_view(self):
        """Get active view or return None."""
        app = Krita.instance()
        window = app.activeWindow()
        if window:
            return window.activeView()
        return None

    def get_active_layer(self):
        """Get active paint layer."""
        doc = self.get_active_document()
        if doc:
            return doc.activeNode()
        return None

    def cmd_new_canvas(self, params):
        """Create a new canvas."""
        width = params.get("width", 800)
        height = params.get("height", 600)
        name = params.get("name", "New Canvas")
        bg_color = params.get("background", "#1a1a2e")

        app = Krita.instance()

        # Create document with background color
        doc = app.createDocument(width, height, name, "RGBA", "U8", "", 120.0)
        print("[KritaMCP] new_canvas: created", flush=True)

        window = app.activeWindow()
        if window:
            view = window.addView(doc)
            if view:
                # addView alone doesn't guarantee activation — undo/redo
                # and other view-scoped commands would hit the wrong doc.
                window.showView(view)
        print("[KritaMCP] new_canvas: view added", flush=True)

        # Create a paint layer
        root = doc.rootNode()
        layer = doc.createNode("paint", "paintlayer")
        root.addChildNode(layer, None)
        print("[KritaMCP] new_canvas: layer added", flush=True)

        # Fill background using pixel data
        color = QColor(bg_color)
        r, g, b = color.red(), color.green(), color.blue()

        # Create pixel data for entire canvas (BGRA format)
        pixel_data = bytes([b, g, r, 255] * (width * height))
        layer.setPixelData(pixel_data, 0, 0, width, height)
        print("[KritaMCP] new_canvas: pixels set", flush=True)

        doc.refreshProjection()
        print("[KritaMCP] new_canvas: refreshed", flush=True)

        return {"status": "ok", "width": width, "height": height, "name": name}

    def cmd_set_color(self, params):
        """Set foreground color."""
        color_hex = params.get("color", "#ffffff")

        view = self.get_active_view()
        if not view:
            return {"error": "No active view"}

        color = QColor(color_hex)
        mc = ManagedColor.fromQColor(color, view.canvas())
        view.setForeGroundColor(mc)

        return {"status": "ok", "color": color_hex}

    def cmd_set_brush(self, params):
        """Set brush preset and size."""
        preset_name = params.get("preset", None)
        size = params.get("size", None)
        opacity = params.get("opacity", None)

        view = self.get_active_view()
        if not view:
            return {"error": "No active view"}

        if preset_name:
            # Find brush preset
            presets = Krita.instance().resources("preset")
            found = None
            for name, preset in presets.items():
                if preset_name.lower() in name.lower():
                    found = preset
                    break
            if found:
                view.setCurrentBrushPreset(found)
            else:
                return {"error": f"Brush preset not found: {preset_name}"}

        if size is not None:
            self.current_brush_size = size
            view.setBrushSize(size)

        if opacity is not None:
            self.current_opacity = opacity
            # Opacity is set per-stroke, store for later

        return {"status": "ok", "preset": preset_name, "size": size, "opacity": opacity}

    def cmd_stroke(self, params):
        """Paint a stroke along points using pixel-level drawing with soft edges."""
        points = params.get("points", [])
        brush_size = params.get("size", self.current_brush_size)
        hardness = params.get("hardness", 0.5)  # 0.0 = very soft, 1.0 = hard edge
        opacity = params.get("opacity", 1.0)

        if len(points) < 2:
            return {"error": "Need at least 2 points for a stroke"}

        layer = self.get_active_layer()
        if not layer:
            return {"error": "No active layer"}

        doc = self.get_active_document()
        view = self.get_active_view()

        if not view:
            return {"error": "No active view"}

        # Get current foreground color
        fg = view.foregroundColor()
        qcolor = fg.colorForCanvas(view.canvas())
        r, g, b = qcolor.red(), qcolor.green(), qcolor.blue()

        width = doc.width()
        height = doc.height()
        radius = max(1, brush_size // 2)

        # Calculate bounding box for all points plus brush radius
        min_x = max(0, int(min(p[0] for p in points)) - radius - 2)
        min_y = max(0, int(min(p[1] for p in points)) - radius - 2)
        max_x = min(width, int(max(p[0] for p in points)) + radius + 2)
        max_y = min(height, int(max(p[1] for p in points)) + radius + 2)

        w = max_x - min_x
        h = max_y - min_y

        if w <= 0 or h <= 0:
            return {"error": "Stroke out of bounds"}

        # Get existing pixel data for the affected region
        existing = layer.pixelData(min_x, min_y, w, h)
        pixels = bytearray(existing)

        import math

        def draw_soft_circle(cx, cy, point_opacity=1.0):
            """Draw a soft circle with falloff at canvas coordinates."""
            for dy in range(-radius, radius + 1):
                for dx in range(-radius, radius + 1):
                    dist_sq = dx*dx + dy*dy
                    if dist_sq <= radius*radius:
                        px = int(cx) + dx - min_x
                        py = int(cy) + dy - min_y
                        if 0 <= px < w and 0 <= py < h:
                            # Calculate distance from center (0.0 to 1.0)
                            dist = math.sqrt(dist_sq) / radius if radius > 0 else 0

                            # Apply hardness curve
                            # hardness=1.0: sharp edge, hardness=0.0: gradual fade from center
                            if hardness >= 1.0:
                                alpha_factor = 1.0
                            else:
                                # Soft falloff: starts fading at hardness point
                                if dist < hardness:
                                    alpha_factor = 1.0
                                else:
                                    # Smooth falloff from hardness to edge
                                    falloff = (dist - hardness) / (1.0 - hardness) if hardness < 1.0 else 0
                                    alpha_factor = 1.0 - falloff

                            final_alpha = int(255 * alpha_factor * opacity * point_opacity)

                            if final_alpha > 0:
                                idx = (py * w + px) * 4
                                # Alpha blending with existing pixel
                                existing_b = pixels[idx]
                                existing_g = pixels[idx+1]
                                existing_r = pixels[idx+2]
                                existing_a = pixels[idx+3]

                                # Simple alpha blend
                                blend = final_alpha / 255.0
                                new_r = int(existing_r * (1 - blend) + r * blend)
                                new_g = int(existing_g * (1 - blend) + g * blend)
                                new_b = int(existing_b * (1 - blend) + b * blend)
                                new_a = max(existing_a, final_alpha)

                                pixels[idx] = new_b
                                pixels[idx+1] = new_g
                                pixels[idx+2] = new_r
                                pixels[idx+3] = new_a

        def draw_line(x1, y1, x2, y2):
            """Draw a line using interpolation with soft brush circles."""
            dist = math.sqrt((x2 - x1)**2 + (y2 - y1)**2)
            # More steps for smoother lines
            steps = max(1, int(dist / max(1, radius / 3)))

            for i in range(steps + 1):
                t = i / steps if steps > 0 else 0
                x = x1 + t * (x2 - x1)
                y = y1 + t * (y2 - y1)
                draw_soft_circle(x, y)

        # Draw soft circles at each point and lines between them
        for i in range(len(points)):
            draw_soft_circle(points[i][0], points[i][1])
            if i > 0:
                draw_line(points[i-1][0], points[i-1][1], points[i][0], points[i][1])

        layer.setPixelData(bytes(pixels), min_x, min_y, w, h)
        doc.refreshProjection()

        return {"status": "ok", "points_count": len(points), "hardness": hardness}

    def cmd_fill(self, params):
        """Fill a circular area with current color."""
        x = params.get("x", 0)
        y = params.get("y", 0)
        radius = params.get("radius", 50)

        layer = self.get_active_layer()
        if not layer:
            return {"error": "No active layer"}

        doc = self.get_active_document()
        view = self.get_active_view()

        if not view:
            return {"error": "No active view"}

        # Get current foreground color
        fg = view.foregroundColor()
        qcolor = fg.colorForCanvas(view.canvas())
        r, g, b = qcolor.red(), qcolor.green(), qcolor.blue()

        # Paint a filled circle using pixel data
        # Create a bounding box
        x1 = max(0, x - radius)
        y1 = max(0, y - radius)
        x2 = min(doc.width(), x + radius)
        y2 = min(doc.height(), y + radius)
        w = x2 - x1
        h = y2 - y1

        if w <= 0 or h <= 0:
            return {"error": "Fill area out of bounds"}

        # Get existing pixel data
        existing = layer.pixelData(x1, y1, w, h)
        pixels = bytearray(existing)

        # Draw circle
        for py in range(h):
            for px in range(w):
                # Check if point is in circle
                dx = (x1 + px) - x
                dy = (y1 + py) - y
                if dx*dx + dy*dy <= radius*radius:
                    idx = (py * w + px) * 4
                    pixels[idx] = b      # B
                    pixels[idx+1] = g    # G
                    pixels[idx+2] = r    # R
                    pixels[idx+3] = 255  # A

        layer.setPixelData(bytes(pixels), x1, y1, w, h)
        doc.refreshProjection()

        return {"status": "ok", "x": x, "y": y, "radius": radius}

    def cmd_draw_shape(self, params):
        """Draw a shape (rectangle, ellipse, line)."""
        shape = params.get("shape", "rectangle")
        x = params.get("x", 0)
        y = params.get("y", 0)
        width = params.get("width", 100)
        height = params.get("height", 100)
        fill = params.get("fill", True)

        layer = self.get_active_layer()
        if not layer:
            return {"error": "No active layer"}

        doc = self.get_active_document()
        view = self.get_active_view()

        if not view:
            return {"error": "No active view"}

        # Get current foreground color
        fg = view.foregroundColor()
        qcolor = fg.colorForCanvas(view.canvas())
        r, g, b = qcolor.red(), qcolor.green(), qcolor.blue()

        if shape == "line":
            # Draw line using pixel data
            x2 = params.get("x2", x + width)
            y2 = params.get("y2", y + height)
            line_width = params.get("line_width", 2)

            # Calculate bounding box
            x1_bound = max(0, int(min(x, x2)) - line_width)
            y1_bound = max(0, int(min(y, y2)) - line_width)
            x2_bound = min(doc.width(), int(max(x, x2)) + line_width)
            y2_bound = min(doc.height(), int(max(y, y2)) + line_width)
            w = x2_bound - x1_bound
            h = y2_bound - y1_bound

            if w > 0 and h > 0:
                existing = layer.pixelData(x1_bound, y1_bound, w, h)
                pixels = bytearray(existing)

                # Draw line with thickness
                dist = max(abs(x2 - x), abs(y2 - y))
                steps = max(1, int(dist))
                radius = max(1, line_width // 2)

                for i in range(steps + 1):
                    t = i / steps if steps > 0 else 0
                    cx = x + t * (x2 - x)
                    cy = y + t * (y2 - y)
                    for dy in range(-radius, radius + 1):
                        for dx in range(-radius, radius + 1):
                            if dx*dx + dy*dy <= radius*radius:
                                px = int(cx) + dx - x1_bound
                                py = int(cy) + dy - y1_bound
                                if 0 <= px < w and 0 <= py < h:
                                    idx = (py * w + px) * 4
                                    pixels[idx] = b
                                    pixels[idx+1] = g
                                    pixels[idx+2] = r
                                    pixels[idx+3] = 255

                layer.setPixelData(bytes(pixels), x1_bound, y1_bound, w, h)
        elif shape == "rectangle" and fill:
            # Draw filled rectangle using pixel data
            x1 = max(0, int(x))
            y1 = max(0, int(y))
            x2 = min(doc.width(), int(x + width))
            y2 = min(doc.height(), int(y + height))
            w = x2 - x1
            h = y2 - y1

            if w > 0 and h > 0:
                pixel_data = bytes([b, g, r, 255] * (w * h))
                layer.setPixelData(pixel_data, x1, y1, w, h)
        elif shape == "ellipse" and fill:
            # Draw filled ellipse using pixel data
            cx = x + width / 2
            cy = y + height / 2
            rx = width / 2
            ry = height / 2

            x1 = max(0, int(x))
            y1 = max(0, int(y))
            x2 = min(doc.width(), int(x + width))
            y2 = min(doc.height(), int(y + height))
            w = x2 - x1
            h = y2 - y1

            if w > 0 and h > 0:
                existing = layer.pixelData(x1, y1, w, h)
                pixels = bytearray(existing)

                for py in range(h):
                    for px in range(w):
                        # Check if point is in ellipse
                        dx = (x1 + px - cx) / rx if rx > 0 else 0
                        dy = (y1 + py - cy) / ry if ry > 0 else 0
                        if dx*dx + dy*dy <= 1:
                            idx = (py * w + px) * 4
                            pixels[idx] = b
                            pixels[idx+1] = g
                            pixels[idx+2] = r
                            pixels[idx+3] = 255

                layer.setPixelData(bytes(pixels), x1, y1, w, h)
        else:
            return {"error": f"Shape '{shape}' with current options not supported"}

        doc.refreshProjection()

        return {"status": "ok", "shape": shape}

    def cmd_get_canvas(self, params):
        """Export current canvas to file and return path."""
        raw_filename = params.get("filename", "canvas.png")
        if not raw_filename or os.path.basename(raw_filename) != raw_filename or "/" in raw_filename or "\\" in raw_filename:
            return {"error": "Invalid filename: must be a bare filename without directories"}
        if ".." in raw_filename:
            return {"error": "Invalid filename: path traversal ('..') not allowed"}

        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}

        filename = raw_filename
        # Ensure filename has extension
        if not filename.endswith('.png'):
            filename += '.png'

        filepath = os.path.join(CANVAS_OUTPUT_DIR, filename)
        filepath, err = _resolve_allowed_path(filepath, for_write=True)
        if err:
            return err

        # Export image (batch mode suppresses export dialog)
        doc.setBatchmode(True)
        doc.exportImage(filepath, InfoObject())
        doc.setBatchmode(False)

        return {"status": "ok", "path": filepath}

    def cmd_undo(self, params):
        """Undo last action."""
        app = Krita.instance()
        action = app.action('edit_undo')
        if action:
            action.trigger()
            return {"status": "ok"}
        return {"error": "Could not trigger undo"}

    def cmd_redo(self, params):
        """Redo last undone action."""
        app = Krita.instance()
        action = app.action('edit_redo')
        if action:
            action.trigger()
            return {"status": "ok"}
        return {"error": "Could not trigger redo"}

    def cmd_screenshot(self, params):
        """Grab the main window to a PNG under CANVAS_OUTPUT_DIR.

        Optional 'zoom' sets the canvas zoom level first (4.0 = 400%)
        so tests can verify actual display filtering, not just the
        document projection.
        """
        raw_filename = params.get("filename", "screenshot.png")
        if not raw_filename or os.path.basename(raw_filename) != raw_filename or "/" in raw_filename or "\\" in raw_filename:
            return {"error": "Invalid filename: must be a bare filename without directories"}
        if ".." in raw_filename:
            return {"error": "Invalid filename: path traversal ('..') not allowed"}

        filename = raw_filename
        if not filename.endswith('.png'):
            filename += '.png'
        filepath = os.path.join(CANVAS_OUTPUT_DIR, filename)
        filepath, err = _resolve_allowed_path(filepath, for_write=True)
        if err:
            return err

        app = Krita.instance()
        window = app.activeWindow()
        if not window:
            return {"error": "No window"}

        zoom = params.get("zoom")
        if zoom is not None:
            view = self.get_active_view()
            if not view:
                return {"error": "No active view"}
            view.canvas().setZoomLevel(float(zoom))
            QApplication.processEvents()

        qwin = window.qwindow()
        if not qwin:
            return {"error": "No qwindow"}

        if params.get("scope") == "canvas":
            # Grab only the canvas widget so window chrome can't pollute
            # pixel analysis. The canvas widget is a C++ class not
            # exported to SIP (KisQPainterCanvas or KisOpenGLCanvas2),
            # so find it via metaObject().className().
            canvas_widget = next(
                (w for w in qwin.findChildren(QWidget)
                 if w.metaObject().className()
                 in ("KisQPainterCanvas", "KisOpenGLCanvas2")),
                None)
            if canvas_widget is None:
                return {"error": "Canvas widget not found"}
            # Queue a repaint and let it land before grabbing — a stale
            # framebuffer after zoom/content changes can come back empty.
            canvas_widget.update()
            QApplication.processEvents()
            image = canvas_widget.grab().toImage()
        elif hasattr(qwin, "grab"):
            image = qwin.grab().toImage()
        else:
            image = qwin.grabWindow().toImage()

        image.save(filepath)
        return {"status": "ok", "path": filepath}

    def cmd_trigger_action(self, params):
        """Trigger an application action by name (e.g. layer_to_vector).

        The action's enabled state syncs on the event loop, so callers
        should verify the effect rather than assume it ran.
        """
        name = params.get("name")
        if not name:
            return {"error": "Missing 'name' parameter"}
        action = Krita.instance().action(name)
        if not action:
            return {"error": f"Unknown action: {name}"}
        action.trigger()
        return {"status": "ok", "action": name}

    def cmd_clear(self, params):
        """Clear the canvas."""
        layer = self.get_active_layer()
        if not layer:
            return {"error": "No active layer"}

        doc = self.get_active_document()

        # Get canvas dimensions
        width = doc.width()
        height = doc.height()

        # Clear by filling with background color
        bg_color = params.get("color", "#1a1a2e")
        color = QColor(bg_color)
        r, g, b = color.red(), color.green(), color.blue()

        # Fill entire layer with color
        pixel_data = bytes([b, g, r, 255] * (width * height))
        layer.setPixelData(pixel_data, 0, 0, width, height)

        doc.refreshProjection()

        return {"status": "ok", "color": bg_color}

    def cmd_save(self, params):
        """Save to specific path."""
        filepath = params.get("path")
        if not filepath:
            return {"error": "No path specified"}

        filepath, err = _resolve_allowed_path(filepath, for_write=True)
        if err:
            return err

        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}

        doc.setBatchmode(True)
        if filepath.lower().endswith((".kra", ".krz")):
            # Native format: saveAs keeps layers; exportImage would flatten.
            doc.saveAs(filepath)
        else:
            doc.exportImage(filepath, InfoObject())
        doc.setBatchmode(False)

        return {"status": "ok", "path": filepath}

    def cmd_get_color_at(self, params):
        """Get color at specific pixel (eyedropper)."""
        x = params.get("x", 0)
        y = params.get("y", 0)

        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}

        # Get projection pixel data at point.
        # QByteArray[i] yields bytes in PyQt6 — coerce to bytes() first so
        # indexing yields ints for the hex format below.
        layer = doc.rootNode()
        pixel_data = bytes(layer.projectionPixelData(x, y, 1, 1))

        if len(pixel_data) >= 4:
            # RGBA
            b, g, r, a = pixel_data[0], pixel_data[1], pixel_data[2], pixel_data[3]
            hex_color = "#{:02x}{:02x}{:02x}".format(r, g, b)
            return {"status": "ok", "color": hex_color, "r": r, "g": g, "b": b, "a": a}

        return {"error": "Could not read pixel"}

    def cmd_list_brushes(self, params):
        """List available brush presets."""
        filter_str = params.get("filter", "")
        limit = params.get("limit", 50)

        presets = Krita.instance().resources("preset")
        brush_list = []

        for name, preset in presets.items():
            if filter_str.lower() in name.lower():
                brush_list.append(name)
                if len(brush_list) >= limit:
                    break

        return {"status": "ok", "brushes": brush_list, "count": len(brush_list)}

    def cmd_open_file(self, params):
        """Open an existing file in Krita."""
        filepath = params.get("path")
        if not filepath:
            return {"error": "No path specified"}

        filepath, err = _resolve_allowed_path(filepath, for_write=False)
        if err:
            return err

        if not os.path.exists(filepath):
            return {"error": f"File not found: {filepath}"}

        app = Krita.instance()

        # Idempotent: re-opening a file that already has a view crashes
        # Krita (stack overflow in the import path). Return the open doc.
        target = os.path.normcase(filepath)
        for existing in app.documents():
            try:
                if existing.fileName() and os.path.normcase(
                    os.path.abspath(existing.fileName())
                ) == target:
                    return {
                        "status": "ok",
                        "path": filepath,
                        "name": existing.name(),
                        "width": existing.width(),
                        "height": existing.height(),
                        "already_open": True,
                    }
            except Exception:
                continue

        # Open the document
        doc = app.openDocument(filepath)
        if not doc:
            return {"error": f"Failed to open: {filepath}"}

        # Add view to active window
        window = app.activeWindow()
        if window:
            view = window.addView(doc)
            if view:
                window.showView(view)

        return {"status": "ok", "path": filepath, "name": doc.name(), "width": doc.width(), "height": doc.height()}

    # ─── LAYER MANAGEMENT ────────────────────────────────────────────

    def _find_node_by_name(self, root, name):
        """Recursively find a node by name."""
        for child in root.childNodes():
            if child.name() == name:
                return child
            found = self._find_node_by_name(child, name)
            if found:
                return found
        return None

    def _node_info(self, node, depth=0):
        """Get info dict for a layer node."""
        info = {
            "name": node.name(),
            "type": node.type(),
            "visible": node.visible(),
            "opacity": node.opacity(),
            "blend_mode": node.blendingMode(),
            "locked": node.locked(),
            "depth": depth,
        }
        if node.childNodes():
            info["children"] = [
                self._node_info(c, depth + 1)
                for c in node.childNodes()
            ]
        return info

    def cmd_add_layer(self, params):
        """Add a new layer to the document."""
        name = params.get("name", "New Layer")
        layer_type = params.get("type", "paintlayer")
        blend_mode = params.get("blend_mode", "normal")
        opacity = params.get("opacity", 255)
        above = params.get("above", None)
        parent_name = params.get("parent", None)

        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}

        node = doc.createNode(name, layer_type)
        node.setBlendingMode(blend_mode)
        node.setOpacity(opacity)

        parent = doc.rootNode()
        if parent_name:
            found = self._find_node_by_name(doc.rootNode(), parent_name)
            if found and found.type() == "grouplayer":
                parent = found

        above_node = None
        if above:
            above_node = self._find_node_by_name(doc.rootNode(), above)

        parent.addChildNode(node, above_node)
        doc.refreshProjection()

        return {
            "status": "ok",
            "name": name,
            "type": layer_type,
            "blend_mode": blend_mode,
            "opacity": opacity,
        }

    def cmd_remove_layer(self, params):
        """Remove a layer by name."""
        name = params.get("name")
        if not name:
            return {"error": "Layer name required"}

        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}

        node = self._find_node_by_name(doc.rootNode(), name)
        if not node:
            return {"error": f"Layer not found: {name}"}

        node.remove()
        doc.refreshProjection()
        return {"status": "ok", "removed": name}

    def cmd_list_layers(self, params):
        """List all layers with full properties."""
        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}

        root = doc.rootNode()
        layers = [self._node_info(c) for c in root.childNodes()]
        active = doc.activeNode()
        active_name = active.name() if active else None

        return {
            "status": "ok",
            "layers": layers,
            "active_layer": active_name,
            "count": len(layers),
        }

    def cmd_set_active_layer(self, params):
        """Set the active layer by name."""
        name = params.get("name")
        if not name:
            return {"error": "Layer name required"}

        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}

        node = self._find_node_by_name(doc.rootNode(), name)
        if not node:
            return {"error": f"Layer not found: {name}"}

        doc.setActiveNode(node)
        return {"status": "ok", "active_layer": name}

    def cmd_set_layer_opacity(self, params):
        """Set opacity for a layer (0-255)."""
        name = params.get("name")
        opacity = params.get("opacity", 255)

        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}

        if name:
            node = self._find_node_by_name(doc.rootNode(), name)
        else:
            node = doc.activeNode()

        if not node:
            return {"error": "Layer not found"}

        node.setOpacity(int(opacity))
        doc.refreshProjection()
        return {"status": "ok", "layer": node.name(), "opacity": opacity}

    def cmd_set_layer_blend_mode(self, params):
        """Set blend mode for a layer."""
        name = params.get("name")
        blend_mode = params.get("blend_mode", "normal")

        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}

        if name:
            node = self._find_node_by_name(doc.rootNode(), name)
        else:
            node = doc.activeNode()

        if not node:
            return {"error": "Layer not found"}

        node.setBlendingMode(blend_mode)
        doc.refreshProjection()
        return {
            "status": "ok",
            "layer": node.name(),
            "blend_mode": blend_mode,
        }

    def cmd_set_layer_visibility(self, params):
        """Show or hide a layer."""
        name = params.get("name")
        visible = params.get("visible", True)

        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}

        if name:
            node = self._find_node_by_name(doc.rootNode(), name)
        else:
            node = doc.activeNode()

        if not node:
            return {"error": "Layer not found"}

        node.setVisible(visible)
        doc.refreshProjection()
        return {
            "status": "ok",
            "layer": node.name(),
            "visible": visible,
        }

    def cmd_move_layer(self, params):
        """Move a layer up, down, or to a specific position."""
        name = params.get("name")
        direction = params.get("direction", "up")

        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}

        if name:
            node = self._find_node_by_name(doc.rootNode(), name)
        else:
            node = doc.activeNode()

        if not node:
            return {"error": "Layer not found"}

        parent = node.parentNode()
        if not parent:
            return {"error": "Cannot move root layer"}

        siblings = parent.childNodes()
        idx = -1
        for i, s in enumerate(siblings):
            if s.name() == node.name():
                idx = i
                break

        if idx < 0:
            return {"error": "Layer index not found"}

        if direction == "up" and idx < len(siblings) - 1:
            above = siblings[idx + 1]
            node.remove()
            parent.addChildNode(node, above)
        elif direction == "down" and idx > 0:
            below_node = siblings[idx - 1] if idx >= 2 else None
            node.remove()
            if below_node:
                parent.addChildNode(node, None)
                # Re-insert below the target
                siblings_new = parent.childNodes()
                for i, s in enumerate(siblings_new):
                    if s.name() == node.name() and i > 0:
                        break
            else:
                parent.addChildNode(node, None)

        doc.refreshProjection()
        return {"status": "ok", "layer": node.name(), "direction": direction}

    def cmd_duplicate_layer(self, params):
        """Duplicate a layer."""
        name = params.get("name")

        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}

        if name:
            node = self._find_node_by_name(doc.rootNode(), name)
        else:
            node = doc.activeNode()

        if not node:
            return {"error": "Layer not found"}

        clone = node.duplicate()
        clone.setName(node.name() + " Copy")
        parent = node.parentNode()
        parent.addChildNode(clone, node)
        doc.refreshProjection()

        return {"status": "ok", "original": node.name(), "copy": clone.name()}

    def cmd_merge_down(self, params):
        """Merge the active layer down."""
        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}

        node = doc.activeNode()
        if not node:
            return {"error": "No active layer"}

        merged = node.mergeDown()
        doc.refreshProjection()

        merged_name = merged.name() if merged else "unknown"
        return {"status": "ok", "merged_into": merged_name}

    # ─── TEMPLATE MANAGEMENT ─────────────────────────────────────────

    def cmd_create_from_template(self, params):
        """Create a new document with a predefined layer structure."""
        preset_name = params.get("preset", "illustration")
        width = params.get("width", 1920)
        height = params.get("height", 1080)
        dpi = params.get("dpi", 300)
        doc_name = params.get("name", "Untitled")
        custom_layers = params.get("layers", None)

        layers = custom_layers or LAYER_PRESETS.get(preset_name)
        if not layers:
            available = list(LAYER_PRESETS.keys())
            return {"error": f"Unknown preset: {preset_name}. Available: {available}"}

        app = Krita.instance()
        doc = app.createDocument(
            width, height, doc_name,
            "RGBA", "U8", "", float(dpi)
        )

        window = app.activeWindow()
        if window:
            view = window.addView(doc)
            if view:
                window.showView(view)

        root = doc.rootNode()

        # Remove default layer if present
        default_nodes = root.childNodes()
        for dn in default_nodes:
            dn.remove()

        # Add layers bottom-to-top (first in list = bottom)
        prev_node = None
        for layer_def in layers:
            ltype = layer_def.get("type", "paintlayer")
            node = doc.createNode(layer_def["name"], ltype)
            node.setBlendingMode(layer_def.get("blend", "normal"))
            node.setOpacity(layer_def.get("opacity", 255))
            root.addChildNode(node, prev_node)
            prev_node = node

        doc.refreshProjection()

        layer_names = [l["name"] for l in layers]
        return {
            "status": "ok",
            "name": doc_name,
            "width": width,
            "height": height,
            "dpi": dpi,
            "preset": preset_name if not custom_layers else "custom",
            "layers": layer_names,
        }

    def cmd_save_as_template(self, params):
        """Save current document as a Krita template."""
        template_name = params.get("name")
        category = params.get("category", "")

        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}

        if not template_name:
            template_name = doc.name() or "Untitled Template"

        # Sanitize name: only [A-Za-z0-9 _-]
        if not re.match(r'^[A-Za-z0-9 _-]+$', template_name):
            return {"error": "Invalid template name: allowed characters are [A-Za-z0-9 _-]"}
        safe_name = template_name.strip()
        if not safe_name:
            return {"error": "Invalid template name"}

        # Sanitize category: only [A-Za-z0-9 _-]
        safe_category = ""
        if category:
            if not re.match(r'^[A-Za-z0-9 _-]+$', category):
                return {"error": "Invalid category name: allowed characters are [A-Za-z0-9 _-]"}
            safe_category = category.strip()

        target_dir = TEMPLATE_DIR
        if safe_category:
            target_dir = os.path.join(TEMPLATE_DIR, safe_category)

        filepath = os.path.join(target_dir, f"{safe_name}.kra")
        filepath, err = _resolve_allowed_path(filepath, for_write=True)
        if err:
            return err

        os.makedirs(os.path.dirname(filepath), exist_ok=True)

        doc.setBatchmode(True)
        doc.saveAs(filepath)
        doc.setBatchmode(False)

        return {
            "status": "ok",
            "path": filepath,
            "name": safe_name,
            "category": safe_category,
        }

    def cmd_list_templates(self, params):
        """List available Krita templates."""
        templates = []

        if not os.path.exists(TEMPLATE_DIR):
            return {"status": "ok", "templates": [], "count": 0}

        for root_dir, dirs, files in os.walk(TEMPLATE_DIR):
            for f in files:
                if f.lower().endswith('.kra'):
                    rel = os.path.relpath(
                        os.path.join(root_dir, f), TEMPLATE_DIR
                    )
                    category = os.path.dirname(rel) or "uncategorized"
                    templates.append({
                        "name": os.path.splitext(f)[0],
                        "category": category,
                        "path": os.path.join(root_dir, f),
                    })

        return {"status": "ok", "templates": templates, "count": len(templates)}

    def cmd_get_layer_presets(self, params):
        """Get available predefined layer stack configurations."""
        preset_name = params.get("name")

        if preset_name:
            preset = LAYER_PRESETS.get(preset_name)
            if not preset:
                return {"error": f"Unknown preset: {preset_name}"}
            return {"status": "ok", "preset": preset_name, "layers": preset}

        result = {}
        for name, layers in LAYER_PRESETS.items():
            result[name] = {
                "layer_count": len(layers),
                "layers": [l["name"] for l in layers],
            }
        return {"status": "ok", "presets": result}

    # ─── DOCUMENT MANAGEMENT ─────────────────────────────────────────

    def cmd_get_document_info(self, params):
        """Get full info about the active document."""
        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}

        root = doc.rootNode()
        layer_count = len(root.childNodes())

        return {
            "status": "ok",
            "name": doc.name(),
            "width": doc.width(),
            "height": doc.height(),
            "resolution": doc.resolution(),
            "color_model": doc.colorModel(),
            "color_depth": doc.colorDepth(),
            "color_profile": doc.colorProfile(),
            "layer_count": layer_count,
            "file_name": doc.fileName(),
            "modified": doc.modified(),
        }

    def cmd_resize_canvas(self, params):
        """Resize the canvas."""
        width = params.get("width")
        height = params.get("height")
        x_offset = params.get("x_offset", 0)
        y_offset = params.get("y_offset", 0)

        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}

        if not width or not height:
            return {"error": "width and height required"}

        doc.resizeImage(x_offset, y_offset, width, height)
        doc.refreshProjection()

        return {
            "status": "ok",
            "width": width,
            "height": height,
        }

    def cmd_scale_image(self, params):
        """Scale the entire image."""
        width = params.get("width")
        height = params.get("height")

        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}

        if not width or not height:
            return {"error": "width and height required"}

        xres = doc.resolution()
        yres = doc.resolution()

        doc.scaleImage(width, height, int(xres), int(yres), "Bicubic")
        doc.refreshProjection()

        return {"status": "ok", "width": width, "height": height}

    def cmd_set_resolution(self, params):
        """Set document DPI."""
        dpi = params.get("dpi", 300)

        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}

        doc.setResolution(int(dpi))
        return {"status": "ok", "dpi": dpi}

    def cmd_flatten_image(self, params):
        """Flatten all layers into one."""
        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}

        doc.flatten()
        doc.refreshProjection()
        return {"status": "ok"}

    # ─── SELECTION ────────────────────────────────────────────────────

    def cmd_select_all(self, params):
        """Select the entire canvas."""
        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}

        from PyQt5.QtCore import QRect
        sel = Selection()
        sel.select(0, 0, doc.width(), doc.height(), 255)
        doc.setSelection(sel)
        return {"status": "ok"}

    def cmd_deselect(self, params):
        """Clear the current selection."""
        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}

        doc.setSelection(None)
        return {"status": "ok"}

    def cmd_select_rect(self, params):
        """Select a rectangular region."""
        x = params.get("x", 0)
        y = params.get("y", 0)
        width = params.get("width", 100)
        height = params.get("height", 100)

        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}

        sel = Selection()
        sel.select(x, y, width, height, 255)
        doc.setSelection(sel)
        return {
            "status": "ok",
            "x": x, "y": y,
            "width": width, "height": height,
        }

    # ─── FILTERS ──────────────────────────────────────────────────────

    def cmd_apply_filter(self, params):
        """Apply a named filter to the active layer."""
        filter_name = params.get("name")
        if not filter_name:
            return {"error": "Filter name required"}

        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}

        node = doc.activeNode()
        if not node:
            return {"error": "No active layer"}

        app = Krita.instance()
        filt = app.filter(filter_name)
        if not filt:
            return {"error": f"Filter not found: {filter_name}"}

        filt.apply(node, 0, 0, doc.width(), doc.height())
        doc.refreshProjection()

        return {"status": "ok", "filter": filter_name}

    def cmd_list_filters(self, params):
        """List available Krita filters."""
        app = Krita.instance()
        filters = app.filters()
        filter_str = params.get("filter", "")

        if filter_str:
            filters = [f for f in filters if filter_str.lower() in f.lower()]

        return {"status": "ok", "filters": filters, "count": len(filters)}

    # ─── EXPORT ───────────────────────────────────────────────────────

    def cmd_export_layers(self, params):
        """Export each visible layer as a separate PNG."""
        output_dir = params.get("output_dir", CANVAS_OUTPUT_DIR)
        prefix = params.get("prefix", "layer")

        output_dir, err = _resolve_allowed_path(output_dir, for_write=True)
        if err:
            return err

        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}

        os.makedirs(output_dir, exist_ok=True)

        root = doc.rootNode()
        exported = []

        for i, node in enumerate(root.childNodes()):
            if not node.visible():
                continue
            safe = "".join(
                c for c in node.name()
                if c.isalnum() or c in (' ', '-', '_')
            ).strip() or f"layer_{i}"
            filepath = os.path.join(output_dir, f"{prefix}_{safe}.png")

            # Get pixel data and save via projection
            pixel_data = node.projectionPixelData(
                0, 0, doc.width(), doc.height()
            )

            # Create a temp document for export
            app = Krita.instance()
            tmp = app.createDocument(
                doc.width(), doc.height(), "tmp",
                "RGBA", "U8", "", doc.resolution()
            )
            tmp_root = tmp.rootNode()
            tmp_layer = tmp.createNode("export", "paintlayer")
            tmp_root.addChildNode(tmp_layer, None)
            tmp_layer.setPixelData(pixel_data, 0, 0, doc.width(), doc.height())
            tmp.refreshProjection()
            tmp.setBatchmode(True)
            tmp.exportImage(filepath, InfoObject())
            tmp.close()

            exported.append({"name": node.name(), "path": filepath})

        return {"status": "ok", "exported": exported, "count": len(exported)}

    def cmd_export_document(self, params):
        """Export the document to a specific format."""
        filepath = params.get("path")
        if not filepath:
            return {"error": "Export path required"}

        filepath, err = _resolve_allowed_path(filepath, for_write=True)
        if err:
            return err

        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}

        doc.setBatchmode(True)
        result = doc.exportImage(filepath, InfoObject())
        doc.setBatchmode(False)

        return {"status": "ok", "path": filepath, "success": result}


# Register the extension
Krita.instance().addExtension(KritaMCPExtension(Krita.instance()))
