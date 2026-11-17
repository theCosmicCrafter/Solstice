"""
Krita MCP Bridge - HTTP server for external paint commands in Krita
Allows Claude (or any MCP client) to paint by sending commands to this plugin.
"""

import hmac
import json
import os
import re
import secrets
import shutil
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs, urlparse
import urllib.request

from krita import *
from PyQt5.QtCore import QPointF, QRectF, QThread, QTimer, pyqtSignal
from PyQt5.QtGui import QColor
from PyQt5.QtWidgets import QApplication, QMdiArea, QMessageBox, QWidget

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
# AGPLv3 §13: network users of this service are entitled to the source.
SOURCE_REPOSITORY = "https://github.com/udtren/Solstice"
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
    # AI generation (vendored ai_diffusion bridge)
    "ai_status", "ai_styles", "ai_generate", "ai_expand",
    "ai_jobs", "ai_apply", "ai_discard", "ai_cancel", "ai_catalog",
    "ai_connect", "ai_install", "ai_update", "ai_update_status",
    "ai_workflows", "ai_workflow",
    "ai_upscale", "ai_remove_distraction", "ai_live",
    "ai_inbetween", "ai_inbetween_status",
    "ai_interpolate", "ai_interpolate_status", "ai_anim",
    "ai_lipsync", "ai_reference", "ai_region",
    "ai_propagate", "ai_propagate_status",
    "ai_animate", "ai_animate_status", "ai_xsheet",
    "ai_h3", "ai_h3_status", "ai_pigment",
    "ai_drag", "ai_drag_status", "canvas_grab", "translate_layer",
    # Export
    "export_layers", "export_document",
    # Dockers
    "show_docker", "list_dockers",
    # Viewport & Camera
    "list_actions", "get_viewport", "set_zoom", "set_rotation",
    "reset_view", "pan_canvas", "set_canvas_mirror", "set_canvas_lod",
]


def _resolve_allowed_path(path, *, for_write=True):
    """Validate and resolve a path against allowed roots.

    Returns (resolved_path, None) on success, or (None, error_dict) on rejection.
    Reads (for_write=False) additionally require the resolved path to exist —
    nonexistent-file reads would only serve as an existence oracle.
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

    if not for_write and not os.path.isfile(resolved):
        return None, {"error": f"File not found: {path}"}

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

# JSON-RPC front for POST /mcp (unified-mcp-design.md). Lazy import of the
# sibling module so plugin load cost stays unchanged for envelope clients.
_mcp_rpc = None


def _mcp():
    global _mcp_rpc
    if _mcp_rpc is None:
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "kritamcp_mcp_rpc", os.path.join(os.path.dirname(__file__), "mcp_rpc.py"))
        _mcp_rpc = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(_mcp_rpc)
    return _mcp_rpc


def _run_command(command):
    """Enqueue one envelope command and block until the GUI-thread worker answers."""
    global command_counter
    command_counter += 1
    command_queue.push(command_counter, command)
    return command_queue.get_result(command_counter)
# Re-entrancy guard for process_commands: several command handlers pump
# the event loop (Document::activeNode -> KisSynchronizedConnectionBase::
# forceDeliverAllSynchronizedEvents -> processEvents), which can deliver
# the next tick of this very QTimer while a command is still running.
# Without a guard each queued command nests one stack level deeper and
# ~30 back-to-back commands overflow the GUI stack (0xc00000fd). Module-
# level so duplicate Extension instances share the guard.
_command_processing = False
# In-flight FLF2V inbetween jobs (comfy prompt_id -> import metadata).
# Module-level like command_queue: multiple Extension instances can be
# registered (duplicate .desktop files) and any of them may process a
# given queued command, so per-request state must not live on self.
_FLF_JOBS = {}

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
        # Second accepted bearer: SOLSTICE_TEST_CONTROL_TOKEN, so the scenario
        # runner and test-control MCP clients can drive the unified port.
        env_tok = _mcp().env_token()
        if provided and env_tok and hmac.compare_digest(provided, env_tok):
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
            resp["source"] = SOURCE_REPOSITORY
            self.send_json_response(resp)
        elif parsed.path == '/source':
            self.send_json_response({
                "license": "AGPL-3.0-only",
                "repository": SOURCE_REPOSITORY,
                "notice": (
                    "Solstice is free software licensed under the AGPLv3. "
                    "Users interacting with it over a network are entitled to "
                    "receive the complete corresponding source."
                ),
            })
        elif parsed.path == '/mcp':
            self.send_response(405)
            self.send_header('Allow', 'POST')
            self.end_headers()
        elif parsed.path == '/info':
            if not self._authorized():
                return
            self.send_json_response({
                "status": "ok",
                "canvas_dir": CANVAS_OUTPUT_DIR,
                "template_dir": TEMPLATE_DIR,
                "commands": list(SUPPORTED_ACTIONS),
                "source": SOURCE_REPOSITORY,
                "mcp": {
                    "endpoint": "/mcp",
                    "protocol_versions": list(_mcp().PROTOCOL_VERSIONS),
                    "native_tools": _mcp().native_tools_available(),
                },
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

        body = self.rfile.read(content_length)
        parsed = urlparse(self.path)

        if parsed.path == '/mcp':
            # Foreign Origin -> 403 (DNS-rebinding guard, upstream parity)
            if not _mcp().origin_allowed(self.headers.get('Origin', '')):
                self.send_json_response({"error": "Forbidden: foreign Origin"}, 403)
                return
            status, payload = _mcp().handle(
                body, run_command=_run_command,
                supported_actions=SUPPORTED_ACTIONS)
            self.send_response(status)
            if payload:
                self.send_header('Content-Type', 'application/json')
            self.end_headers()
            if payload:
                self.wfile.write(payload)
            return

        try:
            command = json.loads(body)
        except (json.JSONDecodeError, UnicodeDecodeError):
            self.send_json_response({"error": "Invalid JSON"}, 400)
            return

        # Assign command ID and queue it; block until the GUI worker answers
        result = _run_command(command)

        if "error" in result:
            self.send_json_response(result, 500)
        else:
            self.send_json_response(result)


class QuietHTTPServer(HTTPServer):
    """HTTPServer that does not dump full tracebacks for routine client
    disconnects.

    A client that closes its socket while the response is being written
    (or a probe that connects and goes away) raises
    ConnectionAbortedError / ConnectionResetError / BrokenPipeError /
    TimeoutError inside the handler. socketserver's default handle_error
    prints a full traceback for each of these, polluting stderr and
    tripping log-cleanliness checks. Real handler bugs still print
    tracebacks.
    """

    def handle_error(self, request, client_address):
        exc_type, exc_value, _tb = sys.exc_info()
        if issubclass(exc_type, (ConnectionError, TimeoutError)):
            print(f"[KritaMCP] client {client_address} went away: {exc_value}",
                  file=sys.stderr, flush=True)
            return
        super().handle_error(request, client_address)


class ServerThread(QThread):
    """Thread to run HTTP server without blocking Krita UI."""

    def __init__(self, port):
        super().__init__()
        self.port = port
        self.server = None

    def run(self):
        global BRIDGE_LISTENING
        try:
            self.server = QuietHTTPServer(('127.0.0.1', self.port), PaintRequestHandler)
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


        # Ensure output directory exists
        os.makedirs(CANVAS_OUTPUT_DIR, exist_ok=True)

        # Start HTTP server immediately on plugin init
        if self.server_thread is None:
            self.server_thread = ServerThread(SERVER_PORT)
            self.server_thread.start()

        # Start timer to process command queue
        if self.timer is None:
            self.timer = QTimer()
            self.timer.timeout.connect(self.process_commands)
            self.timer.start(50)  # Check every 50ms

        try:
            notifier = Krita.instance().notifier()
            notifier.setActive(True)
            notifier.windowCreated.connect(self._on_window_ready)
        except Exception as e:
            print(f"[KritaMCP] windowCreated connect failed: {e}", flush=True)

        # Last-resort fallback: if window signal does not arrive, force-ready
        QTimer.singleShot(5000, self._force_ready)

    def setup(self):
        """Called when extension is loaded."""
        pass

    def createActions(self, window):
        """Called when a new window is created."""
        # Ensure output directory exists
        os.makedirs(CANVAS_OUTPUT_DIR, exist_ok=True)

        # Start HTTP server if not yet running
        if self.server_thread is None:
            self.server_thread = ServerThread(SERVER_PORT)
            self.server_thread.start()

        # Start timer if not yet running
        if self.timer is None:
            self.timer = QTimer()
            self.timer.timeout.connect(self.process_commands)
            self.timer.start(50)

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
        global _command_processing
        if _command_processing:
            # Nested timer tick delivered from an event pump inside the
            # current command — leave queued work for the next outer tick.
            return
        _command_processing = True
        try:
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
        finally:
            _command_processing = False

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
        """Set foreground color. Accepts hex string (color='#rrggbb') or RGB components (r, g, b, [a])."""
        if "color" in params:
            color_val = params["color"]
            color = QColor(color_val) if isinstance(color_val, str) else QColor(*color_val)
        elif "hex" in params:
            color = QColor(params["hex"])
        elif "r" in params and "g" in params and "b" in params:
            a = params.get("a", 255)
            color = QColor(int(params["r"]), int(params["g"]), int(params["b"]), int(a))
        else:
            color = QColor("#ffffff")

        view = self.get_active_view()
        if not view:
            return {"error": "No active view"}
        mc = ManagedColor.fromQColor(color, view.canvas())
        view.setForeGroundColor(mc)

        return {"status": "ok", "color": color.name()}

    def cmd_set_brush(self, params):
        """Set brush preset and size."""
        preset_name = params.get("preset") or params.get("name") or params.get("brush")
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
        raw_points = params.get("points", [])
        points = []
        for p_item in raw_points:
            if isinstance(p_item, dict):
                points.append((float(p_item.get("x", 0)), float(p_item.get("y", 0))))
            elif isinstance(p_item, (list, tuple)) and len(p_item) >= 2:
                points.append((float(p_item[0]), float(p_item[1])))

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
            # so find it via metaObject().className(). Resolve it inside
            # the active MDI subwindow — with several open documents a
            # global findChildren() returns an arbitrary stale view's
            # canvas (e.g. e2e opens ~10 docs before this path).
            canvas_widget = None
            mdi = next(iter(qwin.findChildren(QMdiArea)), None)
            if mdi is not None and mdi.activeSubWindow() is not None:
                canvas_widget = next(
                    (w for w in mdi.activeSubWindow().findChildren(QWidget)
                     if w.metaObject().className()
                     in ("KisQPainterCanvas", "KisOpenGLCanvas2")),
                    None)
            if canvas_widget is None:
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

    def cmd_show_docker(self, params):
        """Show and raise a docker by name or title."""
        name = params.get("name", "")
        if not name:
            return {"error": "Missing 'name' parameter"}
        for d in Krita.instance().dockers():
            title = d.windowTitle()
            obj_name = d.objectName()
            if name.lower() in obj_name.lower() or name.lower() in title.lower():
                d.setVisible(True)
                d.raise_()
                return {"status": "ok", "docker": obj_name, "title": title}
        return {"error": f"Docker '{name}' not found"}

    def cmd_list_dockers(self, params):
        """List all registered dockers and their visibility."""
        res = []
        for d in Krita.instance().dockers():
            res.append({"objectName": d.objectName(), "title": d.windowTitle(), "visible": d.isVisible()})
        return {"status": "ok", "dockers": res}

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
        try:
            if node.animated():
                times = [t for t in range(4096)
                         if node.hasKeyframeAtTime(t)]
                info["animated"] = True
                info["keyframe_times"] = times
        except Exception:
            pass
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
        doc.waitForDone()
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
        blend_mode = params.get("blend_mode") or params.get("mode") or "normal"


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
            below = siblings[idx - 1]
            below.remove()
            parent.addChildNode(below, node)

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

    # ─── AI GENERATION (vendored ai_diffusion bridge) ─────────────────
    #
    # Photoshop-parity generative editing driven through the vendored
    # ai_diffusion plugin, in-process, on the same ComfyUI session the
    # docker uses. Modes for ai_generate:
    #   fill   - prompt-guided inpaint inside the current selection
    #   remove - erase the selection (dedicated inpaint model, no prompt)
    #   expand - generative outpaint; the selection must cover the canvas
    #   create - text to image (the selection is cleared first)
    #   auto   - ai_diffusion resolves txt2img / fill / expand by geometry
    # Results land in the job history. "apply" (default true) inserts the
    # first result as a new layer when the job finishes — the Photoshop
    # behavior — unless the user's finished-action already does that.
    # Poll "ai_jobs", apply any result with "ai_apply", cancel with
    # "ai_cancel".

    _AI_MODES = ("auto", "fill", "remove", "expand", "create")

    def _ai_model(self):
        """The live ai_diffusion DocumentModel for the active document,
        or (None, error-dict) saying what is missing."""
        import sys
        if "ai_diffusion" not in sys.modules:
            # Krita loads enabled plugins at startup; if it's absent the import
            # either failed or the plugin is disabled — retry it to capture
            # the real error for the agent.
            try:
                import importlib
                importlib.import_module("ai_diffusion")
            except Exception as e:
                import traceback
                detail = traceback.format_exc(limit=3).strip().splitlines()[-1]
                return None, {"error": f"ai_diffusion not loaded: {detail}"}
        try:
            from ai_diffusion.model.root import root
            from ai_diffusion.model.connection import ConnectionState
        except Exception as e:
            return None, {"error": f"ai_diffusion import failed: {e}"}
        if not self.get_active_document():
            return None, {"error": "No active document"}
        model = root.model_for_active_document()
        if model is None:
            return None, {"error": "ai_diffusion has no model for the active document"}
        if root.connection.state is not ConnectionState.connected:
            return None, {"error": "ComfyUI is not connected — start it, or open "
                          "the AI Diffusion docker and fix the server settings"}
        return model, None

    def _ai_find_style(self, model, query):
        """Style preset by filename or case-insensitive name; None keeps the
        model's current style."""
        from ai_diffusion.style import Styles
        styles = Styles.list().filtered(show_builtin=True, show_legacy=True)
        found = next(
            (s for s in styles
             if s.filename == query or s.name.lower() == query.lower()), None)
        return found

    _VARIANT_RE = re.compile(r"\[\[([^\[\]]*\|[^\[\]]*)\]\]")
    _VARIANT_CAP = 8    # max distinct prompts from one expansion
    _JOB_CAP = 16       # max jobs a single ai_generate call may enqueue

    @classmethod
    def _expand_variant_specs(cls, prompt):
        """Deterministic `[[a|b|c]]` expansion: every option gets its own
        job (combination product across groups), capped at _VARIANT_CAP.
        Single-brace `{a|b}` is left for the plugin's native seeded
        wildcard evaluation (re-rolled per seed, see eval_wildcards)."""
        groups = cls._VARIANT_RE.findall(prompt)
        print(f"[KritaMCP] variant-expand prompt={prompt!r} groups={groups!r}", flush=True)
        if not groups:
            return [prompt]
        import itertools
        variants = []
        for combo in itertools.product(*(g.split("|") for g in groups)):
            out = prompt
            for g, choice in zip(groups, combo):
                out = cls._VARIANT_RE.sub(choice.strip(), out, count=1)
            variants.append(" ".join(out.split()))
            if len(variants) >= cls._VARIANT_CAP:
                break
        return variants

    def _ai_find_job(self, model, query):
        """Job by id or index — int = position in job list (-1 = latest)."""
        jobs = list(model.jobs)
        if isinstance(query, int) or (isinstance(query, str) and query.lstrip("-").isdigit()):
            idx = int(query)
            return jobs[idx] if -len(jobs) <= idx < len(jobs) else None
        return next((j for j in jobs if j.id == query), None)

    def _ai_auto_apply(self, model, n_before):
        """Apply result 0 of the first job queued after `n_before` once it
        finishes. Skipped when the user's finished-action already applies —
        the model does it itself in `_finish_job`."""
        try:
            from ai_diffusion.settings import GenerationFinishedAction, settings as ai_settings
            if ai_settings.generation_finished_action is GenerationFinishedAction.apply:
                return
        except Exception:
            pass

        watched = {"job": None}

        def _on_finished(job):
            if watched["job"] is not job or job.id is None:
                return
            model.jobs.job_finished.disconnect(_on_finished)
            try:
                model.apply_generated_result(job.id, 0)
            except Exception as e:
                print(f"[KritaMCP] ai_generate auto-apply failed: {e}", flush=True)

        def _on_count():
            if watched["job"] is not None:
                return
            new_jobs = list(model.jobs)[n_before:]
            if not new_jobs:
                return
            watched["job"] = new_jobs[0]
            model.jobs.job_finished.connect(_on_finished)
            model.jobs.count_changed.disconnect(_on_count)

        model.jobs.count_changed.connect(_on_count)
        _on_count()  # the job may already be queued synchronously

    def cmd_ai_status(self, params):
        """ai_diffusion health: enabled, ComfyUI connection, current style,
        queue depth. Safe to call before any other ai_* command."""
        import sys
        if "ai_diffusion" not in sys.modules:
            try:
                import importlib
                importlib.import_module("ai_diffusion")
            except Exception:
                import traceback
                detail = traceback.format_exc(limit=3).strip().splitlines()[-1]
                return {"status": "ok", "enabled": False, "connected": False,
                        "error": f"ai_diffusion not loaded: {detail}"}
        try:
            from ai_diffusion.model.root import root
            from ai_diffusion.model.connection import ConnectionState
        except Exception as e:
            return {"status": "ok", "enabled": True, "connected": False, "error": str(e)}

        state = root.connection.state
        out = {"status": "ok", "enabled": True,
               "connected": state is ConnectionState.connected,
               "state": state.name,
               "build": "ai-bridge-4 (workflows,variants,similar,upscale,live,distraction,frames,lipsync,anim)"}
        try:
            from ai_diffusion.settings import settings as ai_settings
            out["server_url"] = ai_settings.server_url
            mode = ai_settings.server_mode
            out["server_mode"] = mode.name if hasattr(mode, "name") else str(mode)
        except Exception:
            pass
        if state is ConnectionState.connected:
            client = root.connection.client_if_connected
            if client is not None:
                try:
                    out["checkpoints"] = len(client.models.checkpoints)
                    out["loras"] = len(client.models.loras)
                except Exception:
                    pass
                try:
                    info = client.device_info
                    if info is not None:
                        out["gpu"] = {"type": info.type, "name": info.name,
                                      "vram_gb": info.vram}
                except Exception:
                    pass
        try:
            server = root.server
            out["server_state"] = server.state.name
            if server.missing_resources:
                out["missing_resources"] = list(server.missing_resources)
        except Exception:
            pass
        if self.get_active_document():
            model = root.model_for_active_document()
            if model is not None:
                try:
                    from ai_diffusion.model.jobs import JobState
                    out["style"] = getattr(model.style, "name", None)
                    out["jobs_queued"] = model.jobs.count(JobState.queued)
                    out["jobs_executing"] = model.jobs.count(JobState.executing)
                except Exception:
                    pass
        return out

    def cmd_ai_styles(self, params):
        """List style presets usable as 'style' in ai_generate."""
        model, err = self._ai_model()
        if err:
            return err
        try:
            from ai_diffusion.style import Styles
            styles = Styles.list().filtered(show_builtin=True, show_legacy=True)
            return {"status": "ok", "styles": [
                {"name": s.name, "filename": s.filename,
                 "checkpoints": list(getattr(s, "checkpoints", None) or []),
                 "current": s is model.style}
                for s in styles]}
        except Exception as e:
            return {"error": f"style list failed: {e}"}

    def cmd_ai_connect(self, params):
        """Trigger ai_diffusion's managed-server autostart + connect.
        Returns immediately — poll ai_status for state transitions
        (stopped → starting/installing → connected). In managed mode
        this launches ComfyUI; in external/cloud it just connects."""
        import sys
        if "ai_diffusion" not in sys.modules:
            return {"status": "ok", "enabled": False,
                    "error": "ai_diffusion plugin is not enabled"}
        try:
            from ai_diffusion import eventloop
            from ai_diffusion.model.root import root
            from ai_diffusion.model.connection import ConnectionState
            from ai_diffusion.backend.server import ServerState
        except Exception as e:
            return {"status": "ok", "enabled": True, "error": str(e)}
        # A dead managed server leaves connection.state stuck on
        # 'connected' — treat stopped-server as needing a restart.
        if (root.connection.state is ConnectionState.connected
                and root.server.state is not ServerState.stopped):
            return {"status": "ok", "connected": True, "state": "connected"}
        try:
            eventloop.run(root.autostart(lambda: None))
        except Exception as e:
            return {"error": f"autostart failed: {e}"}
        return {"status": "ok", "connected": False, "started": True}

    def cmd_ai_install(self, params):
        """Run the managed installer for missing resources (the bridge
        equivalent of the docker's Install button). Downloads + verifies
        catalog files, installs custom nodes. Poll ai_status for
        server_state/missing_resources and progress via client log."""
        import sys
        if "ai_diffusion" not in sys.modules:
            return {"status": "ok", "enabled": False,
                    "error": "ai_diffusion plugin is not enabled"}
        try:
            from ai_diffusion import eventloop
            from ai_diffusion.model.root import root
            from ai_diffusion.backend.server import ServerState
        except Exception as e:
            return {"status": "ok", "enabled": True, "error": str(e)}
        server = root.server
        packages = params.get("packages")
        if packages:
            try:
                eventloop.run(
                    server.download(list(packages), lambda progress: None))
            except Exception as e:
                return {"error": f"download failed: {e}"}
            return {"status": "ok", "started": True,
                    "packages": list(packages)}
        allowed = {ServerState.not_installed, ServerState.missing_resources,
                   ServerState.update_required}
        if server.state not in allowed:
            return {"status": "ok", "started": False,
                    "server_state": server.state.name,
                    "message": "install not required"}
        try:
            eventloop.run(server.install(lambda progress: None))
        except Exception as e:
            return {"error": f"install failed: {e}"}
        return {"status": "ok", "started": True,
                "missing": list(server.missing_resources)}

    def cmd_ai_update(self, params):
        """Run AI backend maintenance now: apply pending ComfyUI upgrades,
        repair missing custom nodes and required models, and honor the
        configured update channel (managed pins or latest upstream release).
        Returns immediately — poll ai_update_status for state transitions."""
        import sys
        if "ai_diffusion" not in sys.modules:
            return {"status": "ok", "enabled": False,
                    "error": "ai_diffusion plugin is not enabled"}
        try:
            from ai_diffusion import eventloop
            from ai_diffusion.model.root import root
        except Exception as e:
            return {"status": "ok", "enabled": True, "error": str(e)}
        try:
            eventloop.run(root.maintain_server(force=True))
        except Exception as e:
            return {"error": f"update failed: {e}"}
        return {"status": "ok", "maintenance": "started"}

    def cmd_ai_update_status(self, params):
        """Report managed-backend update status: auto-update/channel
        settings, server state + shipped version, installed ComfyUI release,
        last upstream check, missing resources, and native engine health."""
        import sys
        if "ai_diffusion" not in sys.modules:
            return {"status": "ok", "enabled": False,
                    "error": "ai_diffusion plugin is not enabled"}
        try:
            from ai_diffusion.model.root import root
            from ai_diffusion.settings import settings as ai_settings
        except Exception as e:
            return {"status": "ok", "enabled": True, "error": str(e)}
        server = root.server
        out = {"status": "ok", "enabled": True,
               "auto_update": ai_settings.server_auto_update,
               "update_channel": ai_settings.server_update_channel,
               "last_update_check": ai_settings.server_update_checked or None,
               "server_state": server.state.name,
               "server_version": server.version,
               "comfy_version": server.installed_comfy_version(),
               "missing_resources": list(server.missing_resources)}
        try:
            from ai_diffusion.native_engine import find_engine_bundle
            bundle = find_engine_bundle()
            if bundle is None:
                out["native_engine"] = {"bundle": None, "status": "not found"}
            else:
                missing = bundle.check()
                out["native_engine"] = {
                    "bundle": str(bundle.root),
                    "precision": ai_settings.native_precision,
                    "status": "ok" if missing is None else missing}
        except Exception as e:
            out["native_engine"] = {"status": f"check failed: {e}"}
        return out

    def cmd_ai_catalog(self, params):
        """List the managed model catalog: per-resource id/name/arch,
        license, min_vram_gb, files, and installed state. No connection
        needed — reads presets/models.json via backend resources."""
        import sys
        from pathlib import Path
        if "ai_diffusion" not in sys.modules:
            return {"status": "ok", "enabled": False,
                    "error": "ai_diffusion plugin is not enabled"}
        try:
            from ai_diffusion.backend import resources
        except Exception as e:
            return {"status": "ok", "enabled": True, "error": str(e)}
        kinds = {"required": resources.required_models,
                 "checkpoints": resources.default_checkpoints,
                 "upscale": resources.upscale_models,
                 "optional": resources.optional_models}
        installed_dirs = []
        try:
            from ai_diffusion.settings import settings as ai_settings
            if ai_settings.server_path:
                installed_dirs = [ai_settings.server_path]
        except Exception:
            pass
        out = {"status": "ok", "architectures":
               [a.value for a in resources.Arch if a not in
                (resources.Arch.auto, resources.Arch.all)],
               "categories": {}}
        for kind, models in kinds.items():
            entries = []
            for res in models:
                item = {"id": res.id.string, "name": res.name,
                        "arch": res.id.arch.value,
                        "files": [f.path.as_posix() for f in res.files]}
                if res.license:
                    item["license"] = res.license
                if res.min_vram_gb is not None:
                    item["min_vram_gb"] = res.min_vram_gb
                if res.requirements.name != "none":
                    item["requirements"] = res.requirements.name
                if installed_dirs:
                    item["installed"] = any(
                        res.exists_in(Path(d)) for d in installed_dirs)
                entries.append(item)
            out["categories"][kind] = entries
        return out

    def cmd_ai_generate(self, params):
        """Enqueue a generative edit on the active document."""
        model, err = self._ai_model()
        if err:
            return err

        prompt = (params.get("prompt") or "").strip()
        negative = params.get("negative")
        params = dict(params)  # similar_to fills defaults without mutating the caller's dict
        similar = params.get("similar_to")
        if similar is not None:
            sim_job = self._ai_find_job(model, similar)
            if sim_job is None:
                return {"error": f"similar_to: job not found: {similar} (see ai_jobs)"}
            sm = sim_job.params.metadata
            if not prompt:
                prompt = (sm.get("prompt") or "").strip()
            if negative is None and sm.get("negative_prompt"):
                negative = sm["negative_prompt"]
            if "strength" not in params and sm.get("strength") is not None:
                params["strength"] = sm["strength"]
            if "style" not in params and sm.get("style"):
                params["style"] = sm["style"]
        mode = params.get("mode", "auto")
        if mode not in self._AI_MODES:
            return {"error": f"mode must be one of {self._AI_MODES}"}

        doc = self.get_active_document()
        has_selection = doc.selection() is not None
        if mode in ("fill", "remove") and not has_selection:
            return {"error": f"mode '{mode}' needs a selection "
                    "(select_rect / select_all first)"}
        if mode == "expand" and not has_selection:
            return {"error": "mode 'expand' needs a selection covering the "
                    "canvas (usually select_all after growing the canvas — "
                    "or use ai_expand which does both)"}
        if mode == "create" and has_selection:
            doc.setSelection(None)  # create = txt2img; a selection turns it into a fill
            has_selection = False
        if not prompt and mode not in ("remove", "expand"):
            return {"error": "prompt is required unless mode is 'remove' or 'expand'"}

        variant_prompts = self._expand_variant_specs(prompt)
        per_variant = max(1, int(params.get("count", params.get("variations", 1))))
        total_jobs = len(variant_prompts) * per_variant
        if total_jobs > self._JOB_CAP:
            return {"error": f"{len(variant_prompts)} variants x {per_variant} "
                    f"= {total_jobs} jobs (cap {self._JOB_CAP}) — narrow the "
                    "variant spec or lower count"}

        from ai_diffusion.backend.api import InpaintMode
        mode_map = {
            "auto": InpaintMode.automatic,
            "fill": InpaintMode.fill,
            "remove": InpaintMode.remove_object,
            "expand": InpaintMode.expand,
            "create": InpaintMode.automatic,
        }

        style_obj = None
        if style_q := params.get("style"):
            try:
                style_obj = self._ai_find_style(model, str(style_q))
            except Exception as e:
                return {"error": f"style lookup failed: {e}"}
            if style_obj is None:
                return {"error": f"style not found: {style_q} (see ai_styles)"}

        # Mutate model props for this job only — the docker's persisted
        # settings are restored right after enqueue. The prompt stays:
        # it documents what the agent did and lets the user re-run it.
        saved = {p: getattr(model, p) for p in
                 ("strength", "batch_count", "fixed_seed", "seed", "queue_mode", "style")}
        saved_inpaint_mode = model.inpaint.mode
        n_before = len(model.jobs)
        if model.regions is None:
            return {"error": "ai_diffusion has no prompt region — enable "
                    "regional prompts in the AI Diffusion settings"}
        try:
            if negative is not None:
                model.regions.negative = negative
            model.inpaint.mode = mode_map[mode]
            if "strength" in params:
                model.strength = float(params["strength"])
            if "count" in params or "variations" in params:
                model.batch_count = per_variant
            if "seed" in params:
                model.fixed_seed = True
                model.seed = int(params["seed"])
            if style_obj is not None:
                model.style = style_obj
            if queue := params.get("queue"):
                model.queue_mode = type(model.queue_mode)[str(queue)]
            for variant in variant_prompts:
                model.regions.positive = variant
                model.generate()
                if len(variant_prompts) > 1:
                    # replace-mode would cancel the still-queued variant
                    # jobs — force enqueue-after for the rest of the loop
                    model.queue_mode = type(model.queue_mode).back
        except Exception as e:
            return {"error": f"generate failed: {e}"}
        finally:
            for prop, value in saved.items():
                setattr(model, prop, value)
            model.inpaint.mode = saved_inpaint_mode

        # _generate reports fatal errors through model.error and clears it
        # on success; validation_warning is non-fatal (jobs still enqueue).
        err_obj = model.error
        kind_name = getattr(getattr(err_obj, "kind", None), "name", "none")
        err_msg = getattr(err_obj, "message", "") or ""
        if kind_name not in ("none", "validation_warning"):
            return {"error": err_msg or "generation failed", "kind": kind_name}

        # Jobs are appended inside an async enqueue — watch the tail for
        # them instead of assuming they exist yet.
        apply_first = bool(params.get("apply", True))
        if apply_first:
            self._ai_auto_apply(model, n_before)

        return {
            "status": "queued",
            "mode": mode,
            "jobs_from_index": n_before,
            "apply": apply_first,
            "warning": err_msg or None,
            "note": "jobs enqueue asynchronously — poll ai_jobs for the new job id(s)",
        }

    def cmd_ai_remove_distraction(self, params):
        """One-call 'remove distraction': select the marked region
        (rect or point+radius, padded by 'grow'), then generative-fill it
        with mode=remove. Forwards prompt/style/strength/seed/apply to
        ai_generate."""
        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}
        grow = max(0, int(params.get("grow", 8)))
        if rect := params.get("rect"):
            try:
                if isinstance(rect, dict):
                    x, y, w, h = (int(rect[k]) for k in ("x", "y", "w", "h"))
                else:
                    x, y, w, h = (int(v) for v in rect)
            except (KeyError, TypeError, ValueError):
                return {"error": "bad 'rect': expected [x,y,w,h] or "
                        "{x,y,w,h}"}
        elif point := params.get("point"):
            radius = int(params.get("radius", 64))
            x, y, w, h = int(point[0]) - radius, int(point[1]) - radius, radius * 2, radius * 2
        else:
            return {"error": "need 'rect': [x,y,w,h] or 'point': [x,y] "
                    "(+optional 'radius', 'grow')"}
        x, y = x - grow, y - grow
        w, h = w + grow * 2, h + grow * 2
        r = self.cmd_select_rect({"x": x, "y": y, "width": w, "height": h})
        if "error" in r:
            return r
        fwd = {"mode": "remove", "apply": params.get("apply", True)}
        for k in ("prompt", "negative", "style", "strength", "seed", "queue"):
            if k in params:
                fwd[k] = params[k]
        out = self.cmd_ai_generate(fwd)
        out["region"] = {"x": x, "y": y, "w": w, "h": h}
        return out

    def cmd_ai_live(self, params):
        """Toggle/configure the Live (co-generation) workspace: active,
        strength, prompt. Live jobs re-generate when the canvas content
        changes (LiveScheduler polls the region input)."""
        model, err = self._ai_model()
        if err:
            return err
        from ai_diffusion.model.model import Workspace
        try:
            if prompt := params.get("prompt"):
                model.regions.positive = prompt
            if negative := params.get("negative"):
                model.regions.negative = negative
            if "strength" in params:
                model.live.strength = float(params["strength"])
            if style_q := params.get("style"):
                style_obj = self._ai_find_style(model, str(style_q))
                if style_obj is None:
                    return {"error": f"style not found: {style_q} (see ai_styles)"}
                model.style = style_obj
            if "active" in params:
                if bool(params["active"]):
                    model.workspace = Workspace.live
                    model.live.is_active = True
                else:
                    model.live.is_active = False
            if params.get("apply") and model.live.has_result:
                model.live.apply_result(layer_only=bool(params.get("layer_only", True)))
        except Exception as e:
            return {"error": f"ai_live failed: {e}"}
        err_obj = getattr(model, "error", None)
        result_img = getattr(model.live, "_result", None)
        from ai_diffusion.settings import settings as _ai_settings
        return {"status": "ok",
                "workspace": model.workspace.name,
                "live_active": model.live.is_active,
                "doc_active": model.document.is_active,
                "doc_name": getattr(model.document, "name", None),
                "strength": model.live.strength,
                "has_result": model.live.has_result,
                "result_extent": list(result_img.extent) if result_img else None,
                "model_error": getattr(err_obj, "message", None) or None,
                "live_engine": getattr(_ai_settings, "live_engine", "comfyui"),
                "settings_path": str(_ai_settings.default_path),
                "live_engine_raw": _ai_settings._values.get("live_engine"),
                "native_engine_reason": getattr(
                    getattr(model.live, "_native", None), "availability", lambda: None
                )() or None,
                "note": "live jobs regenerate on canvas changes; ai_jobs "
                        "shows kind=live_preview"}

    def cmd_ai_expand(self, params):
        """Photoshop-style Generative Expand in one gesture: grow the canvas
        by the margins, select everything, outpaint the new band."""
        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}
        left = int(params.get("left", 0))
        top = int(params.get("top", 0))
        right = int(params.get("right", 0))
        bottom = int(params.get("bottom", 0))
        if min(left, top, right, bottom) < 0:
            return {"error": "margins must be >= 0"}
        if left + top + right + bottom == 0:
            return {"error": "at least one non-zero margin required"}
        if max(doc.width() + left + right, doc.height() + top + bottom) > 32768:
            return {"error": "resulting canvas too large"}

        w, h = doc.width(), doc.height()
        doc.resizeImage(left, top, w + left + right, h + top + bottom)
        doc.refreshProjection()
        self.cmd_select_all({})
        fwd = dict(params)
        fwd["mode"] = "expand"
        result = self.cmd_ai_generate(fwd)
        if "error" in result:
            return result
        result["canvas"] = {"width": w + left + right, "height": h + top + bottom}
        result["margins"] = {"left": left, "top": top,
                             "right": right, "bottom": bottom}
        return result

    # ---- Wan 2.2 FLF2V inbetweening ----------------------------------------
    #
    # `ai_inbetween` exports two keyframe layers, uploads them to ComfyUI,
    # and queues the verified Wan 2.2 i2v first-last-frame graph (high/low
    # noise split + lightx2v 4-step loras). `ai_inbetween_status` polls the
    # history, downloads decoded frames through /view, and imports them onto
    # a dedicated animation layer via Document.importAnimation.

    _FLF2V_FILES = {
        "unet_high": r"wan\wan2.2_i2v_high_noise_14B_fp8_scaled.safetensors",
        "unet_low": r"wan\wan2.2_i2v_low_noise_14B_fp8_scaled.safetensors",
        "clip": "umt5_xxl_fp16.safetensors",
        "vae": "wan_2.1_vae.safetensors",
        "clip_vision": "wanVideo_clipVisionH.safetensors",
        "lora_high": r"Wan Video 2.2 I2V-A14B\wan2.2_i2v_lightx2v_4steps_lora_v1_high_noise.safetensors",
        "lora_low": r"Wan Video 2.2 I2V-A14B\wan2.2_i2v_lightx2v_4steps_lora_v1_low_noise.safetensors",
    }

    def _ai_comfy_base(self):
        """http(s) base of the managed ComfyUI, from ai_diffusion settings."""
        from ai_diffusion.settings import settings as ai_settings
        url = (ai_settings.server_url or "").strip().rstrip("/")
        if not url:
            return None
        if not url.startswith(("http://", "https://")):
            url = "http://" + url
        return url

    def _comfy_post(self, base, path, payload):
        import json as _json
        req = urllib.request.Request(
            base + path, _json.dumps(payload).encode("utf-8"),
            {"Content-Type": "application/json"})
        return _json.loads(urllib.request.urlopen(req, timeout=120).read())

    def _comfy_upload_png(self, base, name, png_bytes):
        import json as _json
        import uuid
        boundary = uuid.uuid4().hex
        body = (
            f"--{boundary}\r\nContent-Disposition: form-data; name=\"image\"; "
            f"filename=\"{name}\"\r\nContent-Type: image/png\r\n\r\n"
        ).encode() + png_bytes + (
            f"\r\n--{boundary}\r\nContent-Disposition: form-data; "
            f"name=\"overwrite\"\r\n\r\ntrue\r\n"
            f"--{boundary}\r\nContent-Disposition: form-data; "
            f"name=\"type\"\r\n\r\ninput\r\n--{boundary}--\r\n"
        ).encode()
        req = urllib.request.Request(
            base + "/upload/image", body,
            {"Content-Type": f"multipart/form-data; boundary={boundary}"})
        return _json.loads(urllib.request.urlopen(req, timeout=120).read())

    def _export_node_png(self, doc, node, path):
        """Flatten one Krita node to a doc-sized PNG via a throwaway doc.
        projectionPixelData returns bytes in the document's native color
        space — the temp doc must match it or the upload is malformed."""
        pixel_data = node.projectionPixelData(0, 0, doc.width(), doc.height())
        app = Krita.instance()
        tmp = app.createDocument(doc.width(), doc.height(), "flf-tmp",
                                 doc.colorModel(), doc.colorDepth(),
                                 doc.colorProfile(), doc.resolution())
        layer = tmp.createNode("export", "paintlayer")
        tmp.rootNode().addChildNode(layer, None)
        layer.setPixelData(pixel_data, 0, 0, doc.width(), doc.height())
        tmp.refreshProjection()
        tmp.setBatchmode(True)
        tmp.exportImage(path, InfoObject())
        tmp.close()

    def _find_krita_node(self, doc, query):
        if not query:
            return None
        def walk(node):
            for child in node.childNodes():
                yield child
                yield from walk(child)
        for node in walk(doc.rootNode()):
            if node.name() == query or str(node.uniqueId()) == str(query):
                return node
        return None

    def _flf2v_graph(self, first_name, last_name, w, h, length,
                     prompt, negative, seed, steps):
        f = self._FLF2V_FILES
        hi = max(1, steps // 2)
        return {
          "1": {"class_type": "UNETLoader", "inputs": {"unet_name": f["unet_high"], "weight_dtype": "default"}},
          "2": {"class_type": "UNETLoader", "inputs": {"unet_name": f["unet_low"], "weight_dtype": "default"}},
          "3": {"class_type": "CLIPLoader", "inputs": {"clip_name": f["clip"], "type": "wan"}},
          "4": {"class_type": "VAELoader", "inputs": {"vae_name": f["vae"]}},
          "5": {"class_type": "CLIPVisionLoader", "inputs": {"clip_name": f["clip_vision"]}},
          "6": {"class_type": "LoadImage", "inputs": {"image": first_name}},
          "7": {"class_type": "LoadImage", "inputs": {"image": last_name}},
          "8": {"class_type": "CLIPVisionEncode", "inputs": {"clip_vision": ["5", 0], "image": ["6", 0], "crop": "none"}},
          "9": {"class_type": "CLIPVisionEncode", "inputs": {"clip_vision": ["5", 0], "image": ["7", 0], "crop": "none"}},
          "10": {"class_type": "CLIPTextEncode", "inputs": {"clip": ["3", 0], "text": prompt}},
          "11": {"class_type": "CLIPTextEncode", "inputs": {"clip": ["3", 0], "text": negative}},
          "12": {"class_type": "LoraLoader", "inputs": {"model": ["1", 0], "clip": ["3", 0], "lora_name": f["lora_high"], "strength_model": 1.0, "strength_clip": 1.0}},
          "13": {"class_type": "LoraLoader", "inputs": {"model": ["2", 0], "clip": ["3", 0], "lora_name": f["lora_low"], "strength_model": 1.0, "strength_clip": 1.0}},
          "14": {"class_type": "WanFirstLastFrameToVideo", "inputs": {
              "positive": ["10", 0], "negative": ["11", 0], "vae": ["4", 0],
              "width": w, "height": h, "length": length, "batch_size": 1,
              "clip_vision_start_image": ["8", 0], "clip_vision_end_image": ["9", 0],
              "start_image": ["6", 0], "end_image": ["7", 0]}},
          "15": {"class_type": "KSamplerAdvanced", "inputs": {
              "model": ["12", 0], "positive": ["14", 0], "negative": ["14", 1],
              "latent_image": ["14", 2], "add_noise": "enable", "noise_seed": seed,
              "steps": steps, "cfg": 1.0, "sampler_name": "uni_pc",
              "scheduler": "simple", "start_at_step": 0, "end_at_step": hi,
              "return_with_leftover_noise": "enable"}},
          "16": {"class_type": "KSamplerAdvanced", "inputs": {
              "model": ["13", 0], "positive": ["14", 0], "negative": ["14", 1],
              "latent_image": ["15", 0], "add_noise": "disable", "noise_seed": seed,
              "steps": steps, "cfg": 1.0, "sampler_name": "uni_pc",
              "scheduler": "simple", "start_at_step": hi, "end_at_step": steps,
              "return_with_leftover_noise": "disable"}},
          "17": {"class_type": "VAEDecode", "inputs": {"samples": ["16", 0], "vae": ["4", 0]}},
          "19": {"class_type": "SaveImage", "inputs": {"images": ["17", 0], "filename_prefix": f"solstice_inbetween_{seed}"}},
        }

    def cmd_ai_inbetween(self, params):
        """Queue Wan 2.2 FLF2V inbetweening between two keyframe layers.
        Params: first_layer, last_layer (names or ids), frames (4n+1,
        default 9), prompt, negative, seed, steps (default 4),
        first_frame (timeline index for import). Returns a comfy
        prompt_id to poll with ai_inbetween_status."""
        model, err = self._ai_model()
        if err:
            return err
        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}

        first = self._find_krita_node(doc, params.get("first_layer", ""))
        last = self._find_krita_node(doc, params.get("last_layer", ""))
        if first is None or last is None:
            names = [n.name() for n in doc.rootNode().childNodes()]
            return {"error": "first_layer and last_layer required",
                    "layers": names}

        base = self._ai_comfy_base()
        if not base:
            return {"error": "ComfyUI server URL not configured"}

        import tempfile, time
        seed = int(params.get("seed", int(time.time()) % 2**31))
        frames = int(params.get("frames", 9))
        if frames % 4 != 1:
            frames = max(5, frames - (frames % 4) + 1)
        prompt = params.get("prompt", "smooth animation, natural motion")
        negative = params.get("negative", "blurry, distorted, morphing")
        steps = int(params.get("steps", 4))
        w = (doc.width() // 8) * 8
        h = (doc.height() // 8) * 8

        tmpdir = tempfile.mkdtemp(prefix="flf_")
        try:
            for node, tag in ((first, "first"), (last, "last")):
                path = os.path.join(tmpdir, f"{tag}.png")
                self._export_node_png(doc, node, path)
                up = self._comfy_upload_png(
                    base, f"sol_{tag}_{seed}.png", open(path, "rb").read())
            graph = self._flf2v_graph(
                f"sol_first_{seed}.png", f"sol_last_{seed}.png",
                w, h, frames, prompt, negative, seed, steps)
            resp = self._comfy_post(base, "/prompt", {"prompt": graph})
        except Exception as e:
            return {"error": f"comfy request failed: {e}"}
        finally:
            shutil.rmtree(tmpdir, ignore_errors=True)

        if resp.get("error") or not resp.get("prompt_id"):
            return {"error": resp.get("error") or "prompt rejected",
                    "node_errors": resp.get("node_errors")}

        _FLF_JOBS[resp["prompt_id"]] = {
            "first_frame": int(params.get("first_frame", 0)),
            "step": int(params.get("step", 1)),
            "layer_name": params.get("layer_name", "[Inbetween]"),
            "seed": seed, "frames": frames,
        }
        return {"status": "queued", "prompt_id": resp["prompt_id"],
                "frames": frames, "width": w, "height": h,
                "note": "poll ai_inbetween_status with this prompt_id"}

    def cmd_ai_inbetween_status(self, params):
        """Poll an inbetween job. On completion downloads the decoded
        frames via ComfyUI /view and imports them onto a dedicated
        animation layer. Returns pending/running/done/error."""
        return self._frame_job_status(params)

    _H3_FILES = {
        "unet": "fasth3_omniref_8step_pruned_int8_convrot.safetensors",
        "clip": "qwen3vl_32b_minimax_h3_int8_convrot.safetensors",
        "vae": "minimax_h3_video_vae_int8_convrot.safetensors",
        "audio_vae": "minimax_h3_audio_vae_fp32.safetensors",
    }

    def _h3_graph(self, first_name, w, h, length, prompt, negative,
                  seed, steps, audio):
        f = self._H3_FILES
        i2v_in = {
            "clip": ["2", 0], "vae": ["3", 0], "prompt": prompt,
            "width": w, "height": h, "length": length}
        neg_in = dict(i2v_in, prompt=negative)
        if first_name:
            i2v_in["first_frame"] = ["5", 0]
        g = {
          "1": {"class_type": "UNETLoader", "inputs": {"unet_name": f["unet"], "weight_dtype": "default"}},
          "2": {"class_type": "CLIPLoader", "inputs": {"clip_name": f["clip"], "type": "minimax"}},
          "3": {"class_type": "VAELoader", "inputs": {"vae_name": f["vae"]}},
          "4": {"class_type": "VAELoader", "inputs": {"vae_name": f["audio_vae"]}},
          "8": {"class_type": "MiniMaxH3ImageToVideo", "inputs": i2v_in},
          "9": {"class_type": "MiniMaxH3ImageToVideo", "inputs": neg_in},
          "10": {"class_type": "KSampler", "inputs": {
              "model": ["1", 0], "positive": ["8", 0], "negative": ["9", 0],
              "latent_image": ["8", 1], "seed": seed, "steps": steps,
              "cfg": 1.0, "sampler_name": "euler", "scheduler": "simple",
              "denoise": 1.0}},
          "11": {"class_type": "VAEDecode", "inputs": {"samples": ["10", 0], "vae": ["3", 0]}},
          "13": {"class_type": "SaveImage", "inputs": {"images": ["11", 0], "filename_prefix": f"solstice_h3_{seed}"}},
        }
        if first_name:
            g["5"] = {"class_type": "LoadImage", "inputs": {"image": first_name}}
        if audio:
            g["12"] = {"class_type": "VAEDecodeAudio", "inputs": {"samples": ["10", 0], "vae": ["4", 0]}}
            g["14"] = {"class_type": "SaveAudio", "inputs": {"audio": ["12", 0], "filename_prefix": f"solstice_h3_audio_{seed}"}}
        return g

    def cmd_ai_h3(self, params):
        """Queue a MiniMax H3 joint video+audio generation (t2v / i2v).
        Params: prompt, negative, first_layer (optional i2v start frame),
        width/height (default 512x288), frames (17k+5 grid — 22≈1s,
        124≈5s; default 22), seed, steps (8 for fasth3), audio
        (default True), audio_path (where the FLAC lands), first_frame,
        layer_name. Returns a comfy prompt_id for ai_h3_status."""
        model, err = self._ai_model()
        if err:
            return err
        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}

        base = self._ai_comfy_base()
        if not base:
            return {"error": "ComfyUI server URL not configured"}

        import tempfile, time
        seed = int(params.get("seed", int(time.time()) % 2**31))
        frames = int(params.get("frames", 22))
        if frames < 5 or frames % 17 != 5:
            frames = max(5, 5 + 17 * round((frames - 5) / 17))
        w = int(params.get("width", 512)) // 32 * 32
        h = int(params.get("height", 288)) // 32 * 32
        prompt = params.get("prompt", "")
        negative = params.get("negative", "")
        steps = int(params.get("steps", 8))
        audio = bool(params.get("audio", True))
        first_name = None

        src = self._find_krita_node(doc, params.get("first_layer", ""))
        tmpdir = tempfile.mkdtemp(prefix="h3_")
        try:
            if src is not None:
                path = os.path.join(tmpdir, "first.png")
                self._export_node_png(doc, src, path)
                self._comfy_upload_png(
                    base, f"sol_h3_first_{seed}.png",
                    open(path, "rb").read())
                first_name = f"sol_h3_first_{seed}.png"
            graph = self._h3_graph(first_name, w, h, frames, prompt,
                                   negative, seed, steps, audio)
            resp = self._comfy_post(base, "/prompt", {"prompt": graph})
        except Exception as e:
            return {"error": f"comfy request failed: {e}"}
        finally:
            shutil.rmtree(tmpdir, ignore_errors=True)

        if resp.get("error") or not resp.get("prompt_id"):
            return {"error": resp.get("error") or "prompt rejected",
                    "node_errors": resp.get("node_errors")}

        if not params.get("audio_path"):
            audio_path = os.path.join(
                tempfile.gettempdir(), f"solstice_h3_{seed}.flac")
        else:
            audio_path, perr = _resolve_allowed_path(
                params["audio_path"], for_write=True)
            if perr:
                return perr
        _FLF_JOBS[resp["prompt_id"]] = {
            "first_frame": int(params.get("first_frame", 0)),
            "step": int(params.get("step", 1)),
            "layer_name": params.get("layer_name", "[H3]"),
            "seed": seed, "frames": frames,
            "prefix": f"solstice_h3_{seed}",
            "audio": audio, "audio_path": audio_path,
            "audio_prefix": f"solstice_h3_audio_{seed}",
        }
        return {"status": "queued", "prompt_id": resp["prompt_id"],
                "frames": frames, "width": w, "height": h,
                "audio": audio, "audio_path": audio_path,
                "note": "poll ai_h3_status with this prompt_id"}

    def cmd_ai_h3_status(self, params):
        """Poll an ai_h3 job. On done, imports video frames onto the
        named animation layer and saves the generated audio track to
        the job's audio_path."""
        return self._frame_job_status(params)

    def _frame_job_status(self, params):
        """Shared poll+import for queued frame jobs (FLF2V, RIFE).
        Jobs register in _FLF_JOBS with a SaveImage `prefix` — on
        completion the frames are downloaded via /view and imported as
        timeline keyframes on a dedicated animation layer."""
        base = self._ai_comfy_base()
        if not base:
            return {"error": "ComfyUI server URL not configured"}
        pid = params.get("prompt_id")
        meta = _FLF_JOBS.get(pid)
        if not pid or meta is None:
            return {"error": "unknown prompt_id", "known": list(_FLF_JOBS)}

        import json as _json
        try:
            history = _json.loads(urllib.request.urlopen(
                base + f"/history/{pid}", timeout=30).read())
        except Exception as e:
            return {"status": "error", "error": str(e)}
        entry = history.get(pid)
        if entry is None:
            return {"status": "pending", "prompt_id": pid}
        st = entry.get("status", {})
        if not st.get("completed"):
            msgs = [str(m)[:200] for m in st.get("messages", [])[-2:]]
            return {"status": "running", "prompt_id": pid, "messages": msgs}

        prefix = meta.get("prefix", "solstice_inbetween_")
        frame_entries = []
        audio_entries = []
        audio_prefix = meta.get("audio_prefix", "")
        for node_out in entry.get("outputs", {}).values():
            for img in node_out.get("images", []):
                if img.get("filename", "").startswith(prefix):
                    frame_entries.append(img)
            for au in node_out.get("audio", []):
                if au.get("filename", "").startswith(audio_prefix):
                    audio_entries.append(au)
        frame_entries.sort(key=lambda i: i["filename"])
        if not frame_entries:
            return {"status": "error", "error": "no frames in outputs"}

        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}
        import tempfile, urllib.parse as _up
        files = []
        tmpdir = tempfile.mkdtemp(prefix="flf_out_")
        try:
            for i, img in enumerate(frame_entries):
                q = _up.urlencode({"filename": img["filename"],
                                   "subfolder": img.get("subfolder", ""),
                                   "type": img.get("type", "output")})
                data = urllib.request.urlopen(f"{base}/view?{q}", timeout=60).read()
                path = os.path.join(tmpdir, f"frame_{i:04d}.png")
                open(path, "wb").write(data)
                files.append(path)

            # importAnimation creates its own paint layer — snapshot current
            # node ids so the new layer can be found and renamed afterwards.
            before_ids = {str(n.uniqueId()) for n in doc.rootNode().childNodes()}
            ok = doc.importAnimation(files, meta["first_frame"], meta["step"])
            doc.waitForDone()
            if not ok:
                # NOTE: never include an "error" key on success paths — the
                # dispatcher treats any response containing the key as HTTP 500.
                return {"error": "importAnimation failed"}
            for node in doc.rootNode().childNodes():
                if str(node.uniqueId()) not in before_ids:
                    node.setName(meta["layer_name"])
                    break
            result = {"status": "done", "prompt_id": pid,
                      "imported": len(files), "layer": meta["layer_name"],
                      "first_frame": meta["first_frame"], "step": meta["step"]}
            if meta.get("audio") and audio_entries:
                audio_path, perr = _resolve_allowed_path(
                    meta["audio_path"], for_write=True)
                if perr:
                    result["audio_error"] = perr["error"]
                else:
                    au = audio_entries[0]
                    q = _up.urlencode({"filename": au["filename"],
                                       "subfolder": au.get("subfolder", ""),
                                       "type": au.get("type", "output")})
                    data = urllib.request.urlopen(f"{base}/view?{q}", timeout=60).read()
                    open(audio_path, "wb").write(data)
                    result["audio_path"] = audio_path
                    result["audio_bytes"] = len(data)
        finally:
            shutil.rmtree(tmpdir, ignore_errors=True)
        del _FLF_JOBS[pid]
        return result

    def _export_pixels_png(self, doc, pixel_data, path):
        """Write a raw RGBA pixelData buffer to a doc-sized PNG."""
        app = Krita.instance()
        tmp = app.createDocument(doc.width(), doc.height(), "px-tmp",
                                 doc.colorModel(), doc.colorDepth(),
                                 doc.colorProfile(), doc.resolution())
        layer = tmp.createNode("export", "paintlayer")
        tmp.rootNode().addChildNode(layer, None)
        layer.setPixelData(pixel_data, 0, 0, doc.width(), doc.height())
        tmp.refreshProjection()
        tmp.setBatchmode(True)
        tmp.exportImage(path, InfoObject())
        tmp.close()

    def cmd_ai_interpolate(self, params):
        """RIFE densification of an animated layer's keyframes.
        Params: layer (name/id; default: first animated layer),
        multiplier (2-16, default 2), start/end (frame range, default
        all keyframes found), first_frame, step, layer_name.
        Returns a comfy prompt_id for ai_interpolate_status.
        NOTE: RIFE is for dense sequences — sparse keyframes dissolve.
        Use ai_inbetween (FLF2V) for key-to-key generation."""
        model, err = self._ai_model()
        if err:
            return err
        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}

        layer_name = params.get("layer")
        if layer_name:
            node = self._find_krita_node(doc, layer_name)
            if node is None:
                return {"error": f"layer not found: {layer_name}"}
        else:
            def walk(n):
                for c in n.childNodes():
                    yield c
                    yield from walk(c)
            node = next((n for n in walk(doc.rootNode())
                         if n.animated()), None)
            if node is None:
                return {"error": "no animated layer found",
                        "hint": "pass layer= or run ai_inbetween first"}

        start = int(params.get("start", 0))
        end = int(params.get("end", 512))
        times = [t for t in range(start, end + 1)
                 if node.hasKeyframeAtTime(t)]
        if len(times) < 2:
            return {"error": f"need >=2 keyframes on '{node.name()}', "
                    f"found {len(times)}"}
        times = times[:64]  # upload cap
        multiplier = max(2, min(16, int(params.get("multiplier", 2))))

        base = self._ai_comfy_base()
        if not base:
            return {"error": "ComfyUI server URL not configured"}

        import tempfile, time
        tag = int(time.time()) % 2**31
        tmpdir = tempfile.mkdtemp(prefix="rife_")
        try:
            names = []
            for i, t in enumerate(times):
                path = os.path.join(tmpdir, f"kf_{i:03d}.png")
                pd = node.pixelDataAtTime(0, 0, doc.width(), doc.height(), t)
                self._export_pixels_png(doc, pd, path)
                name = f"rife_{tag}_{i:03d}.png"
                self._comfy_upload_png(base, name, open(path, "rb").read())
                names.append(name)

            # Chain ImageBatch nodes into one IMAGE batch for FrameInterpolate.
            g = {}
            prev = None
            for i, name in enumerate(names):
                g[f"load{i}"] = {"class_type": "LoadImage",
                                 "inputs": {"image": name}}
                if prev is None:
                    prev = [f"load{i}", 0]
                else:
                    g[f"batch{i}"] = {"class_type": "ImageBatch", "inputs": {
                                      "image1": prev, "image2": [f"load{i}", 0]}}
                    prev = [f"batch{i}", 0]
            g["3"] = {"class_type": "FrameInterpolationModelLoader",
                      "inputs": {"model_name": "rife_v4.26.safetensors"}}
            g["5"] = {"class_type": "FrameInterpolate", "inputs": {
                      "interp_model": ["3", 0], "images": prev,
                      "multiplier": multiplier}}
            g["6"] = {"class_type": "SaveImage", "inputs": {
                      "images": ["5", 0],
                      "filename_prefix": f"solstice_interp_{tag}"}}

            resp = self._comfy_post(base, "/prompt", {"prompt": g})
        except Exception as e:
            return {"error": f"comfy request failed: {e}"}
        finally:
            shutil.rmtree(tmpdir, ignore_errors=True)

        if resp.get("error") or not resp.get("prompt_id"):
            return {"error": resp.get("error") or "prompt rejected",
                    "node_errors": resp.get("node_errors")}

        _FLF_JOBS[resp["prompt_id"]] = {
            "first_frame": int(params.get("first_frame", 0)),
            "step": int(params.get("step", 1)),
            "layer_name": params.get("layer_name", "[Interpolated]"),
            "prefix": f"solstice_interp_{tag}",
            "keyframes": len(times),
        }
        return {"status": "queued", "prompt_id": resp["prompt_id"],
                "keyframes": len(times), "multiplier": multiplier,
                "source_layer": node.name(),
                "note": "poll ai_interpolate_status with this prompt_id"}

    def cmd_ai_interpolate_status(self, params):
        """Poll an interpolate job — same shared path as
        ai_inbetween_status."""
        return self._frame_job_status(params)

    _RHUBARB_CANDIDATES = [
        "rhubarb", "rhubarb.exe",
    ]
    _RHUBARB_SHAPES = ("A", "B", "C", "D", "E", "F", "G", "H", "X")

    def _rhubarb_binary(self):
        # KRITAMCP_RHUBARB env var wins, then PATH/conventional locations.
        env_path = os.environ.get("KRITAMCP_RHUBARB", "").strip()
        for cand in [env_path] + self._RHUBARB_CANDIDATES if env_path else self._RHUBARB_CANDIDATES:
            if not cand:
                continue
            if os.path.isabs(cand):
                if os.path.isfile(cand):
                    return cand
            else:
                found = shutil.which(cand)
                if found:
                    return found
        return None

    def _mouth_layers(self, doc, group_name=None):
        """Map Preston-Blair shape letters (A-H, X) to paint layers.

        Mouth layers are matched by normalized name: the letter itself,
        or `mouth_<letter>`/`mouth <letter>`/`mouth<letter>` forms
        (case-insensitive). Layers inside a group whose name contains
        'mouth' (or the named group) win over loose layers."""
        def walk(n):
            for c in n.childNodes():
                yield c
                yield from walk(c)

        def letter_of(node):
            name = node.name().strip().upper()
            name = name.replace("MOUTH", "").strip("_ -")
            return name if name in self._RHUBARB_SHAPES else None

        grouped, loose = {}, {}
        for node in walk(doc.rootNode()):
            letter = letter_of(node)
            if letter is None or node.type() != "paintlayer":
                continue
            parent = node.parentNode()
            in_group = False
            if parent and parent.type() == "grouplayer":
                pname = parent.name().lower()
                in_group = ("mouth" in pname
                            or (group_name and pname == group_name.lower()))
            if in_group:
                grouped.setdefault(letter, node)
            else:
                loose.setdefault(letter, node)
        merged = dict(loose)
        merged.update(grouped)
        return merged

    def cmd_ai_lipsync(self, params):
        """Rhubarb lip-sync: audio -> phoneme track -> mouth-shape
        keyframe layer via Document.importAnimation.

        Params: audio (file path), mouth_group (optional group name),
        start_frame (default 0), recognizer (pocketSphinx|phonetic),
        dialog (optional dialog text file path), name (result layer
        name, default '[Lipsync]')."""
        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}
        audio = params.get("audio")
        if audio:
            audio, perr = _resolve_allowed_path(audio, for_write=False)
            if perr:
                return perr
        if not audio or not os.path.isfile(audio):
            return {"error": f"audio file not found: {audio}"}
        rhubarb = self._rhubarb_binary()
        if not rhubarb:
            return {"error": "rhubarb binary not found",
                    "searched": self._RHUBARB_CANDIDATES}

        mouths = self._mouth_layers(doc, params.get("mouth_group"))
        if "X" not in mouths:
            mouths["X"] = mouths.get("A") or next(iter(mouths.values()), None)
        if not mouths:
            return {"error": "no mouth-shape layers found",
                    "hint": "create paint layers named A-H/X inside a "
                            "group named 'Mouths'"}

        import json, subprocess, tempfile
        cmd = [rhubarb, "-f", "json",
               "-r", params.get("recognizer", "pocketSphinx")]
        if params.get("dialog"):
            dialog, perr = _resolve_allowed_path(params["dialog"], for_write=False)
            if perr:
                return perr
            cmd += ["-d", dialog]
        cmd.append(audio)
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True,
                                  timeout=300)
        except subprocess.TimeoutExpired:
            return {"error": "rhubarb timed out after 300s"}
        if proc.returncode != 0:
            return {"error": "rhubarb failed",
                    "stderr": proc.stderr[-500:]}
        cues = json.loads(proc.stdout).get("mouthCues", [])
        if not cues:
            return {"error": "rhubarb produced no mouth cues"}

        fps = doc.framesPerSecond() or 24
        duration = max(c["end"] for c in cues)
        total = max(1, int(round(duration * fps)) + 1)

        work = tempfile.mkdtemp(prefix="solstice_lipsync_")
        try:
            shape_pngs = {}
            for letter, node in mouths.items():
                path = os.path.join(work, f"mouth_{letter}.png")
                self._export_node_png(doc, node, path)
                shape_pngs[letter] = path

            files = []
            for frame in range(total):
                t = frame / fps
                cue = next((c for c in cues
                            if c["start"] <= t < c["end"]), cues[-1])
                files.append(shape_pngs.get(cue["value"],
                                            shape_pngs["X"]))

            n_before = {str(n.uniqueId())
                        for n in doc.rootNode().childNodes()}
            start = int(params.get("start_frame", 0))
            ok = doc.importAnimation(files, start, 1)
            doc.waitForDone()
            if not ok:
                return {"error": "importAnimation failed"}
            name = params.get("name", "[Lipsync]")
            for n in doc.rootNode().childNodes():
                if str(n.uniqueId()) not in n_before:
                    n.setName(name)
            return {"status": "ok", "layer": name, "frames": total,
                    "start_frame": start, "duration": duration,
                    "fps": fps, "cues": len(cues),
                    "shapes_used": sorted(shape_pngs)}
        finally:
            shutil.rmtree(work, ignore_errors=True)

    def cmd_ai_reference(self, params):
        """Assign a Krita image layer as a control-layer input.

        Params: layer (name of the image/reference layer), region
        (optional — name of a region-linked layer; without it the
        control goes on the root region = whole document), mode
        (reference|style|composition|face|scribble|line_art|soft_edge|
        depth|normal|pose|segmentation; default reference), strength
        (0-100), clear (remove existing controls on that target first).
        """
        model, err = self._ai_model()
        if err:
            return err
        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}
        from ai_diffusion.model.control import ControlMode

        node = self._find_krita_node(doc, params.get("layer"))
        if not node:
            return {"error": f"layer not found: {params.get('layer')}"}
        layer = model.layers.find(node.uniqueId())
        if layer is None or not layer.type.is_image:
            return {"error": "layer is not an image layer"}

        if params.get("region"):
            rnode = self._find_krita_node(doc, params["region"])
            rlayer = rnode and model.layers.find(rnode.uniqueId())
            region = rlayer and model.regions.find_linked(rlayer)
            if region is None:
                return {"error": f"no region linked to "
                                f"{params['region']}"}
            target = region.control
        else:
            target = model.regions.control

        if params.get("clear"):
            for c in list(target):
                target.remove(c)

        # ControlLayerList.add() binds the ACTIVE layer — activate ours,
        # add, restore. layer_id is set explicitly anyway.
        prev_active = doc.activeNode()
        doc.setActiveNode(node)
        target.add()
        if prev_active is not None:
            doc.setActiveNode(prev_active)
        ctrl = target[-1]
        mode_name = params.get("mode", "reference")
        try:
            ctrl.mode = ControlMode[mode_name]
        except KeyError:
            return {"error": f"unknown mode: {mode_name}",
                    "modes": [m.name for m in ControlMode]}
        ctrl.layer_id = layer.id
        if "strength" in params:
            ctrl.strength = int(params["strength"])
        return {"status": "ok", "mode": ctrl.mode.name,
                "layer": layer.name, "strength": ctrl.strength,
                "region": params.get("region") or "root",
                "controls": len(target)}

    def cmd_ai_region(self, params):
        """Create/list prompt regions.

        Without params (or `list=True`): list all regions with linked
        layers, prompts, and control counts. With `layer`: create a
        region linked to that layer (or the group containing it —
        `Region.link_target` semantics) and optionally set `positive`.
        `link_layer` adds another layer to an existing region found via
        `of_layer`."""
        model, err = self._ai_model()
        if err:
            return err
        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}
        from ai_diffusion.model.region import Region

        if params.get("list") or not (params.get("layer")
                                      or params.get("link_layer")):
            out = []
            for r in model.regions:
                out.append({"layers": [l.name for l in r.layers],
                            "positive": r.positive,
                            "controls": len(r.control)})
            return {"status": "ok", "regions": out,
                    "count": len(out)}

        if params.get("link_layer"):
            rnode = self._find_krita_node(doc, params["of_layer"])
            rlayer = rnode and model.layers.find(rnode.uniqueId())
            region = rlayer and model.regions.find_linked(rlayer)
            if region is None:
                return {"error": f"no region linked to "
                                f"{params.get('of_layer')}"}
            node = self._find_krita_node(doc, params["link_layer"])
            layer = node and model.layers.find(node.uniqueId())
            if layer is None:
                return {"error": f"layer not found: "
                                 f"{params['link_layer']}"}
            region.link(layer)
        else:
            node = self._find_krita_node(doc, params["layer"])
            layer = node and model.layers.find(node.uniqueId())
            if layer is None:
                return {"error": f"layer not found: {params['layer']}"}
            region = model.regions.emplace()
            region.link(Region.link_target(layer))

        if "positive" in params:
            region.positive = params["positive"]
        return {"status": "ok", "region": region.name,
                "layers": [l.name for l in region.layers],
                "positive": region.positive,
                "regions": len(list(model.regions))}

    _VACE_FILES = {
        "unet": r"wan\wan2.1_vace_14B_fp8_e4m3fn.safetensors",
        "clip": "umt5_xxl_fp16.safetensors",
        "vae": "wan_2.1_vae.safetensors",
    }

    def _vace_graph(self, ctrl_names, ref_name, w, h, length, prompt,
                    negative, seed, steps, strength):
        f = self._VACE_FILES
        g = {
          "1": {"class_type": "UNETLoader", "inputs": {
                "unet_name": f["unet"], "weight_dtype": "default"}},
          "2": {"class_type": "CLIPLoader", "inputs": {
                "clip_name": f["clip"], "type": "wan"}},
          "3": {"class_type": "VAELoader", "inputs": {
                "vae_name": f["vae"]}},
          "4": {"class_type": "CLIPTextEncode", "inputs": {
                "clip": ["2", 0], "text": prompt}},
          "5": {"class_type": "CLIPTextEncode", "inputs": {
                "clip": ["2", 0], "text": negative}},
        }
        prev = None
        for i, name in enumerate(ctrl_names):
            g[f"c{i}"] = {"class_type": "LoadImage",
                          "inputs": {"image": name}}
            if prev is None:
                prev = [f"c{i}", 0]
            else:
                g[f"cb{i}"] = {"class_type": "ImageBatch", "inputs": {
                               "image1": prev, "image2": [f"c{i}", 0]}}
                prev = [f"cb{i}", 0]
        vace_in = {"positive": ["4", 0], "negative": ["5", 0],
                   "vae": ["3", 0], "width": w, "height": h,
                   "length": length, "batch_size": 1,
                   "strength": strength}
        if prev is not None:
            vace_in["control_video"] = prev
        if ref_name:
            g["ref"] = {"class_type": "LoadImage",
                        "inputs": {"image": ref_name}}
            vace_in["reference_image"] = ["ref", 0]
        g["6"] = {"class_type": "WanVaceToVideo", "inputs": vace_in}
        g["7"] = {"class_type": "KSampler", "inputs": {
                  "model": ["1", 0], "positive": ["6", 0],
                  "negative": ["6", 1], "latent_image": ["6", 2],
                  "seed": seed, "steps": steps, "cfg": 1.0,
                  "sampler_name": "uni_pc", "scheduler": "simple",
                  "denoise": 1.0}}
        g["8"] = {"class_type": "VAEDecode", "inputs": {
                  "samples": ["7", 0], "vae": ["3", 0]}}
        return g

    def cmd_ai_propagate(self, params):
        """VACE motion propagation (Phase 12.5): drive a generation with
        a control video extracted from an animated layer (or explicit
        layers) plus an optional appearance reference.

        Params: control_layer (animated layer name), start/end (frame
        range, default 0..64), keyframes_only (sparse control),
        reference_layer (static layer), prompt/negative/seed/steps/
        strength/width/height/length, first_frame, step, layer_name.
        Returns a prompt_id for ai_propagate_status."""
        model, err = self._ai_model()
        if err:
            return err
        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}

        ctrl = self._find_krita_node(doc, params.get("control_layer")) \
            if params.get("control_layer") else None
        if params.get("control_layer") and ctrl is None:
            return {"error": f"control layer not found: "
                             f"{params['control_layer']}"}
        if ctrl is None:
            def walk(n):
                for c in n.childNodes():
                    yield c
                    yield from walk(c)
            ctrl = next((n for n in walk(doc.rootNode())
                         if n.animated()), None)
        if ctrl is None:
            return {"error": "no animated control layer found"}

        start = int(params.get("start", 0))
        end = int(params.get("end", 64))
        if params.get("keyframes_only"):
            times = [t for t in range(start, end + 1)
                     if ctrl.hasKeyframeAtTime(t)]
        else:
            times = list(range(start, end + 1))
        times = times[:81]
        if len(times) < 2:
            return {"error": f"control layer '{ctrl.name()}' needs "
                             f">=2 frames in {start}..{end}"}

        ref_node = (self._find_krita_node(doc, params["reference_layer"])
                    if params.get("reference_layer") else None)
        if params.get("reference_layer") and ref_node is None:
            return {"error": f"reference layer not found: "
                             f"{params['reference_layer']}"}

        base = self._ai_comfy_base()
        if not base:
            return {"error": "ComfyUI server URL not configured"}

        import tempfile, time
        tag = int(time.time()) % 2**31
        tmpdir = tempfile.mkdtemp(prefix="vace_")
        try:
            names = []
            for i, t in enumerate(times):
                path = os.path.join(tmpdir, f"ctl_{i:03d}.png")
                pd = ctrl.pixelDataAtTime(0, 0, doc.width(),
                                        doc.height(), t)
                self._export_pixels_png(doc, pd, path)
                name = f"vace_{tag}_ctl{i:03d}.png"
                self._comfy_upload_png(base, name,
                                       open(path, "rb").read())
                names.append(name)

            ref_name = None
            if ref_node is not None:
                ref_path = os.path.join(tmpdir, "ref.png")
                self._export_node_png(doc, ref_node, ref_path)
                ref_name = f"vace_{tag}_ref.png"
                self._comfy_upload_png(base, ref_name,
                                       open(ref_path, "rb").read())

            def snap(v, step):
                return max(step, (int(v) // step) * step)
            w = snap(params.get("width", doc.width()), 16)
            h = snap(params.get("height", doc.height()), 16)
            length = int(params.get("length", len(times)))
            length = max(5, ((length - 1) // 4) * 4 + 1)  # Wan 4n+1
            g = self._vace_graph(
                names, ref_name, w, h, length,
                params.get("prompt", ""),
                params.get("negative", ""),
                int(params.get("seed", 0)),
                int(params.get("steps", 8)),
                float(params.get("strength", 1.0)))
            g["9"] = {"class_type": "SaveImage", "inputs": {
                      "images": ["8", 0],
                      "filename_prefix": f"solstice_vace_{tag}"}}
            resp = self._comfy_post(base, "/prompt", {"prompt": g})
        except Exception as e:
            return {"error": f"comfy request failed: {e}"}
        finally:
            shutil.rmtree(tmpdir, ignore_errors=True)

        if resp.get("error") or not resp.get("prompt_id"):
            return {"error": resp.get("error") or "prompt rejected",
                    "node_errors": resp.get("node_errors")}

        _FLF_JOBS[resp["prompt_id"]] = {
            "first_frame": int(params.get("first_frame", 0)),
            "step": int(params.get("step", 1)),
            "layer_name": params.get("layer_name", "[Propagated]"),
            "prefix": f"solstice_vace_{tag}",
        }
        return {"status": "queued", "prompt_id": resp["prompt_id"],
                "control_frames": len(names), "control": ctrl.name(),
                "reference": bool(ref_name), "size": f"{w}x{h}",
                "length": length,
                "note": "poll ai_propagate_status with this prompt_id"}

    def cmd_ai_propagate_status(self, params):
        """Poll a VACE job — same shared path as ai_inbetween_status."""
        return self._frame_job_status(params)

    _WAN_ANIMATE_FILES = {
        "unet": r"wan\wan2.2_animate_14B_bf16.safetensors",
        "clip": "umt5_xxl_fp16.safetensors",
        "vae": "wan_2.1_vae.safetensors",
        "clip_vision": "wanVideo_clipVisionH.safetensors",
    }

    def _animate_graph(self, pose_names, ref_name, w, h, length, prompt,
                       negative, seed, steps, pose_strength,
                       ref_strength):
        f = self._WAN_ANIMATE_FILES
        g = {
          "1": {"class_type": "UNETLoader", "inputs": {
                "unet_name": f["unet"], "weight_dtype": "default"}},
          "2": {"class_type": "CLIPLoader", "inputs": {
                "clip_name": f["clip"], "type": "wan"}},
          "3": {"class_type": "VAELoader", "inputs": {
                "vae_name": f["vae"]}},
          "4": {"class_type": "CLIPTextEncode", "inputs": {
                "clip": ["2", 0], "text": prompt}},
          "5": {"class_type": "CLIPTextEncode", "inputs": {
                "clip": ["2", 0], "text": negative}},
          "cv": {"class_type": "CLIPVisionLoader", "inputs": {
                 "clip_name": f["clip_vision"]}},
        }
        prev = None
        for i, name in enumerate(pose_names):
            g[f"p{i}"] = {"class_type": "LoadImage",
                          "inputs": {"image": name}}
            if prev is None:
                prev = [f"p{i}", 0]
            else:
                g[f"pb{i}"] = {"class_type": "ImageBatch", "inputs": {
                               "image1": prev, "image2": [f"p{i}", 0]}}
                prev = [f"pb{i}", 0]
        g["rv"] = {"class_type": "CLIPVisionEncode", "inputs": {
                   "clip_vision": ["cv", 0], "image": ["refimg", 0],
                   "crop": "none"}}
        g["refimg"] = {"class_type": "LoadImage",
                       "inputs": {"image": ref_name}}
        anim_in = {"positive": ["4", 0], "negative": ["5", 0],
                   "vae": ["3", 0], "width": w, "height": h,
                   "length": length, "batch_size": 1,
                   "video_frame_offset": 0,
                   "pose_strength": pose_strength,
                   "pose_start_percent": 0.0, "pose_end_percent": 1.0,
                   "reference_image_strength": ref_strength,
                   "reference_image": ["refimg", 0],
                   "clip_vision_output": ["rv", 0]}
        if prev is not None:
            anim_in["pose_video"] = prev
            g["pv"] = {"class_type": "CLIPVisionEncode", "inputs": {
                       "clip_vision": ["cv", 0], "image": ["p0", 0],
                       "crop": "none"}}
            anim_in["clip_vision_output_pose"] = ["pv", 0]
        g["6"] = {"class_type": "WanAnimate2ToVideo", "inputs": anim_in}
        g["7"] = {"class_type": "KSampler", "inputs": {
                  "model": ["1", 0], "positive": ["6", 0],
                  "negative": ["6", 1], "latent_image": ["6", 2],
                  "seed": seed, "steps": steps, "cfg": 1.0,
                  "sampler_name": "uni_pc", "scheduler": "simple",
                  "denoise": 1.0}}
        g["8"] = {"class_type": "VAEDecode", "inputs": {
                  "samples": ["7", 0], "vae": ["3", 0]}}
        return g

    def cmd_ai_animate(self, params):
        """Wan 2.2 Animate performance transfer (Phase 12.7): drive a
        character/reference image with motion from an animated layer.

        Params: pose_layer (animated layer name; default: first
        animated), reference_layer (character image layer, required),
        start/end/keyframes_only, prompt/negative/seed/steps,
        pose_strength, reference_strength, width/height/length,
        first_frame, step, layer_name ('[Animated]').
        Returns a prompt_id for ai_animate_status."""
        model, err = self._ai_model()
        if err:
            return err
        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}

        pose = self._find_krita_node(doc, params.get("pose_layer")) \
            if params.get("pose_layer") else None
        if params.get("pose_layer") and pose is None:
            return {"error": f"pose layer not found: "
                             f"{params['pose_layer']}"}
        if pose is None:
            def walk(n):
                for c in n.childNodes():
                    yield c
                    yield from walk(c)
            pose = next((n for n in walk(doc.rootNode())
                         if n.animated()), None)
        if pose is None:
            return {"error": "no animated pose layer found"}

        ref_node = (self._find_krita_node(doc, params["reference_layer"])
                    if params.get("reference_layer") else None)
        if ref_node is None:
            return {"error": "reference_layer (character image) required"}

        start = int(params.get("start", 0))
        end = int(params.get("end", 64))
        if params.get("keyframes_only"):
            times = [t for t in range(start, end + 1)
                     if pose.hasKeyframeAtTime(t)]
        else:
            times = list(range(start, end + 1))
        times = times[:81]
        if len(times) < 2:
            return {"error": f"pose layer '{pose.name()}' needs >=2 "
                             f"frames in {start}..{end}"}

        base = self._ai_comfy_base()
        if not base:
            return {"error": "ComfyUI server URL not configured"}

        import tempfile, time
        tag = int(time.time()) % 2**31
        tmpdir = tempfile.mkdtemp(prefix="wanim_")
        try:
            names = []
            for i, t in enumerate(times):
                path = os.path.join(tmpdir, f"pose_{i:03d}.png")
                pd = pose.pixelDataAtTime(0, 0, doc.width(),
                                        doc.height(), t)
                self._export_pixels_png(doc, pd, path)
                name = f"wanim_{tag}_p{i:03d}.png"
                self._comfy_upload_png(base, name,
                                       open(path, "rb").read())
                names.append(name)
            ref_path = os.path.join(tmpdir, "ref.png")
            self._export_node_png(doc, ref_node, ref_path)
            ref_name = f"wanim_{tag}_ref.png"
            self._comfy_upload_png(base, ref_name,
                                   open(ref_path, "rb").read())

            def snap(v, step):
                return max(step, (int(v) // step) * step)
            w = snap(params.get("width", doc.width()), 16)
            h = snap(params.get("height", doc.height()), 16)
            length = int(params.get("length", len(times)))
            length = max(5, ((length - 1) // 4) * 4 + 1)
            g = self._animate_graph(
                names, ref_name, w, h, length,
                params.get("prompt", ""), params.get("negative", ""),
                int(params.get("seed", 0)),
                int(params.get("steps", 8)),
                float(params.get("pose_strength", 1.0)),
                float(params.get("reference_strength", 1.0)))
            g["9"] = {"class_type": "SaveImage", "inputs": {
                      "images": ["8", 0],
                      "filename_prefix": f"solstice_anim_{tag}"}}
            resp = self._comfy_post(base, "/prompt", {"prompt": g})
        except Exception as e:
            return {"error": f"comfy request failed: {e}"}
        finally:
            shutil.rmtree(tmpdir, ignore_errors=True)

        if resp.get("error") or not resp.get("prompt_id"):
            return {"error": resp.get("error") or "prompt rejected",
                    "node_errors": resp.get("node_errors")}

        _FLF_JOBS[resp["prompt_id"]] = {
            "first_frame": int(params.get("first_frame", 0)),
            "step": int(params.get("step", 1)),
            "layer_name": params.get("layer_name", "[Animated]"),
            "prefix": f"solstice_anim_{tag}",
        }
        return {"status": "queued", "prompt_id": resp["prompt_id"],
                "pose_frames": len(names), "pose": pose.name(),
                "reference": ref_node.name(), "size": f"{w}x{h}",
                "length": length,
                "note": "poll ai_animate_status with this prompt_id"}

    def cmd_ai_animate_status(self, params):
        """Poll a Wan Animate job — same shared path."""
        return self._frame_job_status(params)

    def cmd_ai_xsheet(self, params):
        """Exposure-sheet export (Phase 12.1): dump every animated
        layer's keyframe times — and optionally frame thumbnails — to
        JSON or CSV.

        Params: path (output file; .json or .csv by extension),
        thumbnails (bool — export a PNG per keyframe next to the
        file), layer (optional name filter), start/end (time range)."""
        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}
        out_path = params.get("path")
        if not out_path:
            return {"error": "path required (.json or .csv)"}
        out_path, err = _resolve_allowed_path(out_path, for_write=True)
        if err:
            return err

        def walk(n):
            for c in n.childNodes():
                yield c
                yield from walk(c)
        start = int(params.get("start", 0))
        end = int(params.get("end", 4096))
        want = params.get("layer")
        layers = []
        for n in walk(doc.rootNode()):
            try:
                if not n.animated():
                    continue
            except Exception:
                continue
            if want and n.name() != want:
                continue
            times = [t for t in range(start, end + 1)
                     if n.hasKeyframeAtTime(t)]
            layers.append({"name": n.name(), "keyframes": times})

        import json as _json, csv as _csv
        thumbs = bool(params.get("thumbnails"))
        if thumbs:
            tdir = os.path.splitext(out_path)[0] + "_thumbs"
            os.makedirs(tdir, exist_ok=True)
            for lay in layers:
                node = self._find_krita_node(doc, lay["name"])
                safe = "".join(c if c.isalnum() or c in "_-"
                               else "_" for c in lay["name"])
                lay["thumbnails"] = []
                for t in lay["keyframes"]:
                    pd = node.pixelDataAtTime(
                        0, 0, doc.width(), doc.height(), t)
                    tp = os.path.join(tdir, f"{safe}_{t:05d}.png")
                    self._export_pixels_png(doc, pd, tp)
                    lay["thumbnails"].append(tp)

        if out_path.lower().endswith(".csv"):
            with open(out_path, "w", newline="", encoding="utf-8") as f:
                w = _csv.writer(f)
                w.writerow(["layer", "frame"])
                for lay in layers:
                    for t in lay["keyframes"]:
                        w.writerow([lay["name"], t])
        else:
            with open(out_path, "w", encoding="utf-8") as f:
                _json.dump({"fps": doc.framesPerSecond(),
                            "layers": layers}, f, indent=1)
        return {"status": "ok", "path": out_path,
                "layers": len(layers),
                "keyframes": sum(len(l["keyframes"]) for l in layers),
                "thumbnails": thumbs}

    def cmd_translate_layer(self, params):
        """Deterministic pixel move — shift a layer's pixels by (dx,dy).
        This is the 'pixel move' ai_drag is NOT: no generation, exact
        pixel data preserved. Params: layer (default active), dx, dy,
        wrap (wrap edges instead of clipping, default False)."""
        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}
        node = self._find_krita_node(doc, params.get("layer", ""))
        if node is None:
            node = self.get_active_layer()
        if node is None:
            return {"error": "No layer"}
        dx = int(params.get("dx", 0)); dy = int(params.get("dy", 0))
        if dx == 0 and dy == 0:
            return {"error": "dx/dy required (nonzero)"}
        try:
            import numpy as np
        except Exception as e:
            return {"error": f"numpy unavailable: {e}"}
        w, h = doc.width(), doc.height()
        raw = bytes(node.pixelData(0, 0, w, h))
        # treat pixels as opaque units of bpp bytes — works for any
        # color model/depth (RGBA, CMYK, 16/32-bit, ...)
        total = w * h
        if len(raw) < total or len(raw) % total:
            return {"error": f"unexpected pixel buffer size {len(raw)} "
                             f"for {w}x{h}"}
        bpp = len(raw) // total
        pd = np.frombuffer(raw, dtype=np.uint8).reshape(h, w, bpp).copy()
        if params.get("wrap"):
            out = np.roll(pd, (dy, dx), axis=(0, 1))
        else:
            out = np.zeros_like(pd)
            xs = slice(max(0, dx), min(w, w + dx))
            ys = slice(max(0, dy), min(h, h + dy))
            sxs = slice(max(0, -dx), max(0, -dx) + (xs.stop - xs.start))
            sys_ = slice(max(0, -dy), max(0, -dy) + (ys.stop - ys.start))
            if xs.stop > xs.start and ys.stop > ys.start:
                out[ys, xs] = pd[sys_, sxs]
        # Node::setPixelDataUndoable routes the write through a
        # KisTransaction (native libkis addition); fall back to the raw
        # write on builds that predate it and flag the result.
        write = getattr(node, "setPixelDataUndoable", None)
        undoable = write is not None
        if write is None:
            write = node.setPixelData
        write(out.tobytes(), 0, 0, w, h)
        doc.refreshProjection()
        return {"status": "ok", "layer": node.name(), "dx": dx, "dy": dy,
                "undoable": undoable}

    def cmd_ai_drag(self, params):
        """Generative point-drag (Phase 13.3 via FramePainter):
        re-interprets the frame through SVD priors so content deforms
        toward drag targets — NOT a pixel move (see translate_layer
        for that). Params: layer (default active), sx,sy,tx,ty
        (single drag) OR points:[{sx,sy,tx,ty,radius?}] (multi-drag),
        radius (handle size, default 32), width/height (default doc
        snapped to 64), seed, steps (25), guidance_scale (3.0),
        control_scale (0.8), layer_name. Returns a prompt_id for
        ai_drag_status."""
        model, err = self._ai_model()
        if err:
            return err
        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}
        base = self._ai_comfy_base()
        if not base:
            return {"error": "ComfyUI server URL not configured"}

        import tempfile, time
        seed = int(params.get("seed", int(time.time()) % 2**31))
        radius = int(params.get("radius", 32))
        points = params.get("points")
        if not points:
            sx = int(params.get("sx", -1)); sy = int(params.get("sy", -1))
            tx = int(params.get("tx", -1)); ty = int(params.get("ty", -1))
            if min(sx, sy, tx, ty) < 0:
                return {"error": "sx,sy,tx,ty or points[] required"}
            points = [{"sx": sx, "sy": sy, "tx": tx, "ty": ty}]
        for p in points:
            if min(int(p.get("sx", -1)), int(p.get("sy", -1)),
                   int(p.get("tx", -1)), int(p.get("ty", -1))) < 0:
                return {"error": "each point needs sx, sy, tx, ty"}
        # FramePainter accepts 256..1920 in steps of 64 — clamp doc
        # dimensions into range, rounding to the nearest multiple.
        def _fp_dim(v):
            v = int(round(int(v) / 64.0)) * 64
            return min(1920, max(256, v))
        w = _fp_dim(params.get("width", doc.width()))
        h = _fp_dim(params.get("height", doc.height()))

        src = self._find_krita_node(doc, params.get("layer", ""))
        if src is None:
            src = self.get_active_layer()
        if src is None:
            return {"error": "No source layer"}

        tmpdir = tempfile.mkdtemp(prefix="drag_")
        try:
            # 1) source image
            img_path = os.path.join(tmpdir, "src.png")
            self._export_node_png(doc, src, img_path)
            img_name = f"sol_drag_src_{seed}.png"
            self._comfy_upload_png(base, img_name,
                                   open(img_path, "rb").read())
            # 2) drag-track mask: disc at each source + tapered stroke
            #    to its target (union for multi-drag)
            from PIL import Image as _Img, ImageDraw as _ID
            m = _Img.new("L", (doc.width(), doc.height()), 0)
            d = _ID.Draw(m)
            for p in points:
                sx, sy = int(p["sx"]), int(p["sy"])
                tx, ty = int(p["tx"]), int(p["ty"])
                r = int(p.get("radius", radius))
                d.ellipse([sx - r, sy - r, sx + r, sy + r], fill=255)
                d.line([sx, sy, tx, ty], fill=255, width=max(4, r // 2))
                d.ellipse([tx - r // 3, ty - r // 3,
                           tx + r // 3, ty + r // 3], fill=255)
            mp = os.path.join(tmpdir, "mask.png")
            m.save(mp)
            mask_name = f"sol_drag_mask_{seed}.png"
            self._comfy_upload_png(base, mask_name,
                                   open(mp, "rb").read())

            graph = {
              "1": {"class_type": "CheckpointLoaderSimple", "inputs":
                    {"ckpt_name": "svd_xt.safetensors"}},
              "2": {"class_type": "CLIPVisionLoader", "inputs":
                    {"clip_name": "clip-vision_vit-h.safetensors"}},
              "3": {"class_type": "LoadImage", "inputs": {"image": img_name}},
              "4": {"class_type": "LoadImageMask", "inputs":
                    {"image": mask_name, "channel": "red"}},
              "5": {"class_type": "FramePainter_Loader", "inputs": {
                    "model": ["1", 0],
                    "FramePainter_unet": "unet_diffusion_pytorch_model.safetensors",
                    "sparse_control_ckpt": "encoder_diffusion_pytorch_model.safetensors"}},
              "6": {"class_type": "FramePainter_Sampler", "inputs": {
                    "model": ["5", 0], "clip_vision": ["2", 0],
                    "vae": ["1", 2], "image": ["3", 0], "mask": ["4", 0],
                    "seed": seed,
                    "steps": int(params.get("steps", 25)),
                    "guidance_scale": float(params.get("guidance_scale", 3.0)),
                    "width": w, "height": h,
                    "control_scale": float(params.get("control_scale", 0.8))}},
              "7": {"class_type": "SaveImage", "inputs": {
                    "images": ["6", 0],
                    "filename_prefix": f"solstice_drag_{seed}"}},
            }
            resp = self._comfy_post(base, "/prompt", {"prompt": graph})
        except Exception as e:
            return {"error": f"comfy request failed: {e}"}
        finally:
            shutil.rmtree(tmpdir, ignore_errors=True)
        if resp.get("error") or not resp.get("prompt_id"):
            return {"error": resp.get("error") or "prompt rejected",
                    "node_errors": resp.get("node_errors")}
        _FLF_JOBS[resp["prompt_id"]] = {
            "prefix": f"solstice_drag_{seed}",
            "layer_name": params.get("layer_name", "[Dragged]"),
            "drag_single": True,
            "gen_w": w, "gen_h": h,
        }
        return {"status": "queued", "prompt_id": resp["prompt_id"],
                "width": w, "height": h,
                "note": "poll ai_drag_status with this prompt_id"}

    def cmd_ai_drag_status(self, params):
        """Poll an ai_drag job; on done downloads the result image into
        a new paint layer."""
        base = self._ai_comfy_base()
        if not base:
            return {"error": "ComfyUI server URL not configured"}
        pid = params.get("prompt_id")
        meta = _FLF_JOBS.get(pid)
        if not pid or meta is None:
            return {"error": "unknown prompt_id", "known": list(_FLF_JOBS)}
        import json as _json, urllib.parse as _up
        try:
            history = _json.loads(urllib.request.urlopen(
                base + f"/history/{pid}", timeout=30).read())
        except Exception as e:
            return {"status": "error", "error": str(e)}
        entry = history.get(pid)
        if entry is None:
            return {"status": "pending", "prompt_id": pid}
        st = entry.get("status", {})
        if not st.get("completed"):
            msgs = [str(m)[:200] for m in st.get("messages", [])[-2:]]
            return {"status": "running", "prompt_id": pid, "messages": msgs}
        prefix = meta.get("prefix", "solstice_drag_")
        imgs = []
        for node_out in entry.get("outputs", {}).values():
            for img in node_out.get("images", []):
                if img.get("filename", "").startswith(prefix):
                    imgs.append(img)
        if not imgs:
            return {"status": "error", "error": "no image in outputs"}
        img = imgs[0]
        q = _up.urlencode({"filename": img["filename"],
                           "subfolder": img.get("subfolder", ""),
                           "type": img.get("type", "output")})
        data = urllib.request.urlopen(f"{base}/view?{q}", timeout=60).read()
        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}
        if doc.colorModel() != "RGBA" or doc.colorDepth() != "U8":
            return {"error": f"drag import supports U8 RGBA documents only "
                             f"(doc is {doc.colorModel()}/{doc.colorDepth()})"}
        from PyQt5.QtGui import QImage
        qi = QImage.fromData(data, "PNG").convertToFormat(
            QImage.Format_RGBA8888)
        if qi.isNull():
            return {"error": "result PNG failed to decode"}
        # FramePainter ran at snapped dimensions — rescale the result
        # back to the document size before compositing.
        dw, dh = doc.width(), doc.height()
        if qi.width() != dw or qi.height() != dh:
            from PyQt5.QtCore import Qt
            qi = qi.scaled(dw, dh,
                           Qt.AspectRatioMode.IgnoreAspectRatio,
                           Qt.TransformationMode.SmoothTransformation)
        # QImage RGBA8888 supplies straight-alpha R,G,B,A — Krita U8 RGBA
        # paint devices store premultiplied B,G,R,A: swap the end channels
        # and premultiply before writing (setPixelData records undo since
        # the libkis transaction change).
        import numpy as np
        px = np.frombuffer(
            bytes(qi.constBits().asarray(qi.sizeInBytes())), dtype=np.uint8
        ).reshape(-1, 4).copy()
        px[:, [0, 2]] = px[:, [2, 0]]
        a = px[:, 3].astype(np.uint16)[:, None]
        px[:, :3] = ((px[:, :3].astype(np.uint16) * a + 127) // 255).astype(np.uint8)
        layer = doc.createNode(meta.get("layer_name", "[Dragged]"),
                               "paintlayer")
        doc.rootNode().addChildNode(layer, None)
        layer.setPixelData(px.tobytes(), 0, 0, qi.width(), qi.height())
        doc.refreshProjection()
        del _FLF_JOBS[pid]
        return {"status": "done", "layer": layer.name(),
                "width": qi.width(), "height": qi.height(),
                "prompt_id": pid}

    def cmd_ai_pigment(self, params):
        """Pigment blend (Phase 11.1 Python slice): Kubelka-Munk
        spectral mixing — blue+yellow comes out green, white+black
        stays light. Params: top (layer name; default active layer),
        bottom (layer name; default: first paintable layer below top),
        opacity (0..1), layer_name (output, default '[Pigment]')."""
        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}

        top = self._find_krita_node(doc, params.get("top", ""))
        if top is None:
            top = self.get_active_layer()
        if top is None:
            return {"error": "No top layer"}
        if doc.colorModel() != "RGBA" or doc.colorDepth() != "U8":
            return {"error": f"ai_pigment supports U8 RGBA documents only "
                             f"(doc is {doc.colorModel()}/{doc.colorDepth()})"}

        # sibling search + output insertion must happen within the top
        # layer's own parent — nested groups are not root children
        parent = top.parentNode() or doc.rootNode()
        bottom = self._find_krita_node(doc, params.get("bottom", ""))
        if bottom is None:
            sibs = parent.childNodes()
            idx = next((i for i, s in enumerate(sibs)
                        if s.uniqueId() == top.uniqueId()), -1)
            for s in sibs[idx + 1:] if idx >= 0 else sibs:
                if s.type() == "paintlayer":
                    bottom = s
                    break
        if bottom is None:
            return {"error": "No bottom layer to blend with"}

        try:
            import numpy as np
            from . import spectral_mix
        except Exception as e:
            return {"error": f"spectral_mix unavailable: {e}"}

        w, h = doc.width(), doc.height()
        b_bgra = np.frombuffer(
            bytes(bottom.pixelData(0, 0, w, h)), dtype=np.uint8
        ).reshape(h, w, 4)
        t_bgra = np.frombuffer(
            bytes(top.pixelData(0, 0, w, h)), dtype=np.uint8
        ).reshape(h, w, 4)
        # BGRA -> RGBA, blend (premultiplied, tiled internally),
        # RGBA -> BGRA
        out = spectral_mix.blend_layers(
            b_bgra[..., [2, 1, 0, 3]], t_bgra[..., [2, 1, 0, 3]],
            float(params.get("opacity", 1.0)))
        out_bgra = np.ascontiguousarray(out[..., [2, 1, 0, 3]])

        name = params.get("layer_name", "[Pigment]")
        layer = doc.createNode(name, "paintlayer")
        parent.addChildNode(layer, top)
        layer.setPixelData(out_bgra.tobytes(), 0, 0, w, h)
        doc.refreshProjection()
        return {"status": "ok", "layer": name,
                "top": top.name(), "bottom": bottom.name()}

    def cmd_ai_anim(self, params):
        """Animation timeline info + control. Setters: fps, frame
        (current time), play_start/play_end (playback range),
        full_start/full_end (clip range). Always returns the full
        timeline state plus animated layers and their keyframes."""
        doc = self.get_active_document()
        if not doc:
            return {"error": "No active document"}
        if params.get("fps"):
            doc.setFramesPerSecond(int(params["fps"]))
        if "frame" in params:
            doc.setCurrentTime(int(params["frame"]))
        if "play_start" in params or "play_end" in params:
            doc.setPlayBackRange(
                int(params.get("play_start", doc.playBackStartTime())),
                int(params.get("play_end", doc.playBackEndTime())))
        if "full_start" in params:
            doc.setFullClipRangeStartTime(int(params["full_start"]))
        if "full_end" in params:
            doc.setFullClipRangeEndTime(int(params["full_end"]))
        doc.waitForDone()

        def walk(n):
            for c in n.childNodes():
                yield c
                yield from walk(c)
        animated = []
        for n in walk(doc.rootNode()):
            try:
                if n.animated():
                    keys = [t for t in range(4096)
                            if n.hasKeyframeAtTime(t)]
                    animated.append({"name": n.name(), "keyframes": keys})
            except Exception:
                pass
        return {
            "status": "ok",
            "fps": doc.framesPerSecond(),
            "current_time": doc.currentTime(),
            "playback_range": [doc.playBackStartTime(),
                               doc.playBackEndTime()],
            "full_clip_range": [doc.fullClipRangeStartTime(),
                                doc.fullClipRangeEndTime()],
            "animation_length": doc.animationLength(),
            "animated_layers": animated,
        }

    def cmd_ai_workflows(self, params):
        """List custom workflows (from workflows/*.json) with their
        parameter names so callers know what ai_workflow accepts."""
        model, err = self._ai_model()
        if err:
            return err
        custom = model.custom
        out = []
        for wf in custom._workflows:
            entry = {"id": wf.id, "name": wf.name, "source": wf.source.name}
            if wf.id == custom.workflow_id:
                entry["params"] = [
                    {"name": md.name, "kind": md.kind.name, "default": md.default}
                    for md in custom.metadata
                ]
            out.append(entry)
        return {"status": "ok", "workflows": out, "current": custom.workflow_id}

    def cmd_ai_workflow(self, params):
        """Run a custom workflow by id or name. `params.params` maps
        parameter names to values: style params take a style name,
        image/mask-layer params take a layer name or id, text/number/
        toggle params take raw values. Layers default to the top image
        layer when omitted."""
        model, err = self._ai_model()
        if err:
            return err

        query = params.get("workflow")
        if not query:
            return {"error": "workflow id or name required (see ai_workflows)"}
        custom = model.custom
        wf = custom._workflows.find(str(query))
        if wf is None:
            wf = next((w for w in custom._workflows
                       if w.name.lower() == str(query).lower()), None)
        if wf is None:
            ids = [w.id for w in custom._workflows]
            return {"error": f"workflow not found: {query}", "available": ids}

        from ai_diffusion.model.custom_workflow import (
            CustomGenerationMode, ParamKind)
        from ai_diffusion.model.model import Workspace
        from PyQt6.QtCore import QUuid

        n_before = len(model.jobs)
        try:
            custom.workflow_id = wf.id
            if custom.validation_error:
                return {"error": custom.validation_error}

            user_params = params.get("params", {}) or {}
            merged = dict(custom.params)
            used = []
            for md in custom.metadata:
                if md.name not in user_params:
                    continue
                value = user_params[md.name]
                if md.kind in (ParamKind.image_layer, ParamKind.mask_layer):
                    pool = (model.layers.images if md.kind is ParamKind.image_layer
                            else model.layers.masks)
                    layer = next(
                        (l for l in pool
                         if l.name == value or str(l.id) == str(value)), None)
                    if layer is None:
                        return {"error": f"layer not found for '{md.name}': {value}"}
                    value = str(layer.id)
                merged[md.name] = value
                used.append(md.name)
            custom.params = merged
            custom.mode = CustomGenerationMode.regular
            # NOTE: state is intentionally left set — generate() schedules
            # _generate_custom on the asyncio loop and returns immediately,
            # so restoring here would revert workspace/workflow_id before the
            # coroutine reads them and the job would silently never enqueue.
            model.workspace = Workspace.custom
            model.custom.generate()
        except Exception as e:
            return {"error": f"workflow failed: {e}"}

        err_obj = model.error
        kind_name = getattr(getattr(err_obj, "kind", None), "name", "none")
        err_msg = getattr(err_obj, "message", "") or ""
        if kind_name not in ("none", "validation_warning"):
            return {"error": err_msg or "workflow execution failed",
                    "kind": kind_name}

        apply_first = bool(params.get("apply", True))
        if apply_first:
            self._ai_auto_apply(model, n_before)

        return {
            "status": "queued",
            "workflow": wf.id,
            "params_used": used,
            "jobs_from_index": n_before,
            "apply": apply_first,
            "warning": err_msg or None,
            "note": "jobs enqueue asynchronously — poll ai_jobs, then ai_apply",
        }

    def cmd_ai_upscale(self, params):
        """Upscale the active document. `model`: upscaler name —
        "SeedVR2" for generative restoration, or an ESRGAN filename.
        `factor` 1-4 (SeedVR2 only does 2x/4x internally). `refine`
        adds the diffusion refine pass (ignored for SeedVR2).
        `color_correction`: wavelet|lab|adain|none (SeedVR2 only).
        NOTE: resizes the document — the canvas grows by `factor`."""
        model, err = self._ai_model()
        if err:
            return err

        client = model._connection.client_if_connected
        upscaler = str(params.get("model") or "SeedVR2")
        factor = float(params.get("factor", 2.0))
        if factor <= 0 or factor > 4:
            return {"error": "factor must be in (0, 4]"}

        if upscaler != "SeedVR2":
            avail = list(client.models.upscalers) if client else []
            match = next((u for u in avail
                          if u == upscaler or u.lower() == upscaler.lower()
                          or upscaler.lower() in u.lower()), None)
            if match is None:
                return {"error": f"upscaler not found: {upscaler}",
                        "available": avail}
            upscaler = match
        elif client is not None and "SeedVR2" not in client.models.upscalers:
            client.models.upscalers.append("SeedVR2")

        cc = params.get("color_correction")
        if cc is not None:
            if cc not in ("wavelet", "lab", "adain", "none"):
                return {"error": "color_correction must be wavelet|lab|adain|none"}
            from ai_diffusion.ui.upscale import UpscaleWidget
            UpscaleWidget._active_color_correction = cc

        n_before = len(model.jobs)
        extent_before = tuple(model.document.extent)
        try:
            model.upscale.upscaler = upscaler
            model.upscale.factor = factor
            model.upscale.use_diffusion = bool(
                params.get("refine", upscaler != "SeedVR2"))
            model.upscale_image()
        except Exception as e:
            return {"error": f"upscale failed: {e}"}

        err_obj = model.error
        kind_name = getattr(getattr(err_obj, "kind", None), "name", "none")
        err_msg = getattr(err_obj, "message", "") or ""
        if kind_name not in ("none", "validation_warning"):
            return {"error": err_msg or "upscale failed", "kind": kind_name}

        apply_first = bool(params.get("apply", True))
        if apply_first:
            self._ai_auto_apply(model, n_before)

        return {
            "status": "queued",
            "upscaler": upscaler,
            "factor": factor,
            "jobs_from_index": n_before,
            "apply": apply_first,
            "note": f"document will resize {extent_before} -> "
                    f"~x{factor}; poll get_document_info for the new size",
        }

    def cmd_ai_jobs(self, params):
        """List ai_diffusion jobs for the active document (newest first)."""
        model, err = self._ai_model()
        if err:
            return err
        limit = int(params.get("limit", 10))
        state_filter = params.get("state")
        out = []
        for i, job in enumerate(model.jobs):
            if state_filter and job.state.name != state_filter:
                continue
            out.append({
                "index": i,
                "id": job.id,
                "kind": job.kind.name,
                "state": job.state.name,
                "name": job.params.name,
                "seed": job.params.seed,
                "results": len(job.results),
                "used": sorted(k for k, v in job.in_use.items() if v),
            })
        return {"status": "ok", "jobs": out[-limit:][::-1], "count": len(out)}

    def cmd_ai_apply(self, params):
        """Apply one result of a finished job as a new layer — the
        Photoshop 'this is the variation I keep' step. job_id or queue
        index; defaults to the latest finished job, result 0."""
        model, err = self._ai_model()
        if err:
            return err
        job = None
        job_id = params.get("job_id") or params.get("id")
        if job_id:
            job = model.jobs.find(str(job_id))
        elif "index" in params:
            idx = int(params["index"])
            entries = list(model.jobs)
            if 0 <= idx < len(entries):
                job = entries[idx]
        else:
            for j in reversed(list(model.jobs)):
                if j.state.name == "finished" and len(j.results) > 0:
                    job = j
                    break
        if job is None:
            return {"error": "job not found (pass job_id or index; see ai_jobs)"}
        if job.state.name != "finished" or len(job.results) == 0:
            return {"error": f"job is {job.state.name} with {len(job.results)} "
                    "result(s) — nothing to apply yet"}
        if job.id is None:
            return {"error": "job has no server id yet — wait for it to finish"}
        index = int(params.get("result", params.get("result_index", 0)))
        if index >= len(job.results):
            return {"error": f"result index out of range "
                    f"(job has {len(job.results)} results)"}
        model.apply_generated_result(job.id, index)
        return {"status": "ok", "job_id": job.id, "result": index}

    def cmd_ai_discard(self, params):
        """Discard one result of a job — the delete-variation step. A job
        whose last result is discarded is removed from the history."""
        model, err = self._ai_model()
        if err:
            return err
        job_id = params.get("job_id") or params.get("id")
        if not job_id:
            return {"error": "job_id required (see ai_jobs)"}
        job = model.jobs.find(str(job_id))
        if job is None:
            return {"error": f"job not found: {job_id}"}
        if len(job.results) == 0:
            return {"error": "job has no results to discard"}
        if job.id is None:
            return {"error": "job has no server id yet"}
        index = int(params.get("result", params.get("result_index", 0)))
        if index >= len(job.results):
            return {"error": f"result index out of range "
                    f"(job has {len(job.results)} results)"}
        model.jobs.discard(job.id, index)
        return {"status": "ok", "job_id": job.id, "results": len(job.results)}

    def cmd_ai_cancel(self, params):
        """Cancel running and/or queued ai_diffusion jobs on the document."""
        model, err = self._ai_model()
        if err:
            return err
        model.cancel(active=bool(params.get("active", True)),
                     queued=bool(params.get("queued", True)))
        return {"status": "ok"}

    # ─── EXPORT ───────────────────────────────────────────────────────

    def cmd_export_layers(self, params):
        """Export each visible layer as a separate PNG."""
        output_dir = params.get("output_dir", CANVAS_OUTPUT_DIR)
        # The prefix goes into each output filename — restrict it to the same
        # character set as layer names so it cannot escape output_dir.
        prefix = "".join(
            c for c in str(params.get("prefix", "layer"))
            if c.isalnum() or c in (' ', '-', '_')
        ).strip() or "layer"

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

        doc.waitForDone()
        doc.setBatchmode(True)
        cfg = InfoObject()
        if filepath.lower().endswith(".png"):
            cfg.setProperty("alpha", True)
            cfg.setProperty("compression", 6)
        result = doc.exportImage(filepath, cfg)
        doc.setBatchmode(False)

        return {"status": "ok", "path": filepath, "success": result}



    def cmd_list_actions(self, params):
        """List all supported bridge actions."""
        return {"status": "ok", "actions": sorted(SUPPORTED_ACTIONS)}

    def cmd_get_viewport(self, params):
        """Get current canvas viewport state: zoom, rotation, center, mirror."""
        view = self.get_active_view()
        if not view:
            return {"error": "No active view"}
        canvas = view.canvas()
        if not canvas:
            return {"error": "No active canvas"}
        zoom = canvas.zoomLevel() if hasattr(canvas, "zoomLevel") else 1.0
        rot = canvas.rotation() if hasattr(canvas, "rotation") else 0.0
        center = canvas.preferredCenter() if hasattr(canvas, "preferredCenter") else None
        mirror = canvas.mirror() if hasattr(canvas, "mirror") else False
        wrap = canvas.wrapAroundMode() if hasattr(canvas, "wrapAroundMode") else False
        lod = canvas.levelOfDetailMode() if hasattr(canvas, "levelOfDetailMode") else False

        return {
            "status": "ok",
            "zoom_level": zoom,
            "zoom_percent": round(zoom * 100.0, 1),
            "rotation": rot,
            "center": {"x": center.x(), "y": center.y()} if center else None,
            "mirror": mirror,
            "wraparound": wrap,
            "instant_preview_lod": lod,
        }

    def cmd_set_zoom(self, params):
        """Set viewport zoom level. Accepts 'percent' (e.g. 50, 100, 200) or 'level' (e.g. 1.0)."""
        view = self.get_active_view()
        if not view:
            return {"error": "No active view"}
        canvas = view.canvas()
        if not canvas:
            return {"error": "No active canvas"}

        if "level" in params:
            lvl = float(params["level"])
        elif "percent" in params:
            lvl = float(params["percent"]) / 100.0
        elif "zoom" in params:
            z = params["zoom"]
            if isinstance(z, (int, float)):
                lvl = float(z) / 100.0 if z > 5 else float(z)
            elif z == "fit":
                return self.cmd_trigger_action({"name": "zoom_to_fit"})
            elif z == "100":
                return self.cmd_trigger_action({"name": "zoom_to_100pct"})
            else:
                lvl = 1.0
        else:
            lvl = 1.0

        canvas.setZoomLevel(lvl)
        return {
            "status": "ok",
            "zoom_level": canvas.zoomLevel(),
            "zoom_percent": round(canvas.zoomLevel() * 100.0, 1),
        }

    def cmd_set_rotation(self, params):
        """Set viewport rotation in degrees."""
        angle = float(params.get("angle", params.get("rotation", 0.0)))
        view = self.get_active_view()
        if not view:
            return {"error": "No active view"}
        canvas = view.canvas()
        if not canvas:
            return {"error": "No active canvas"}
        canvas.setRotation(angle)
        return {"status": "ok", "rotation": canvas.rotation()}

    def cmd_reset_view(self, params):
        """Reset zoom to 100% and rotation to 0."""
        view = self.get_active_view()
        if not view:
            return {"error": "No active view"}
        canvas = view.canvas()
        if not canvas:
            return {"error": "No active canvas"}
        canvas.resetZoom()
        canvas.resetRotation()
        return {
            "status": "ok",
            "zoom_percent": round(canvas.zoomLevel() * 100.0, 1),
            "rotation": canvas.rotation(),
        }

    def cmd_pan_canvas(self, params):
        """Pan canvas by offset (dx, dy) or set center (x, y)."""
        view = self.get_active_view()
        if not view:
            return {"error": "No active view"}
        canvas = view.canvas()
        if not canvas:
            return {"error": "No active canvas"}

        if "x" in params and "y" in params:
            canvas.setPreferredCenter(QPointF(float(params["x"]), float(params["y"])))
        elif "dx" in params and "dy" in params:
            from PyQt5.QtCore import QPoint
            canvas.pan(QPoint(int(params["dx"]), int(params["dy"])))
        else:
            return {"error": "Expected (x, y) for center or (dx, dy) for pan offset"}

        center = canvas.preferredCenter()
        return {
            "status": "ok",
            "center": {"x": center.x(), "y": center.y()} if center else None,
        }

    def cmd_set_canvas_mirror(self, params):
        """Set horizontal canvas mirror (flip view)."""
        mirror = bool(params.get("mirror", True))
        view = self.get_active_view()
        if not view:
            return {"error": "No active view"}
        canvas = view.canvas()
        if not canvas:
            return {"error": "No active canvas"}
        canvas.setMirror(mirror)
        return {"status": "ok", "mirror": canvas.mirror()}

    def cmd_canvas_grab(self, params):
        """Capture the actual canvas widget as the user sees it (Qt
        grab — includes current zoom/filter state). Params: path
        (PNG output). Returns path + widget size."""
        view = self.get_active_view()
        if not view:
            return {"error": "No active view"}
        path = params.get("path")
        if not path:
            return {"error": "path required"}
        path, err = _resolve_allowed_path(path, for_write=True)
        if err:
            return err
        # libkis Canvas isn't a QWidget — find the real canvas widget
        # inside the main window and grab() that.
        app = Krita.instance()
        qw = app.activeWindow().qwindow() if app.activeWindow() else None
        if qw is None:
            return {"error": "No window"}
        target = None
        stack = [qw]
        while stack:
            w = stack.pop()
            cname = w.metaObject().className()
            if "Canvas" in cname or "canvas" in (w.objectName() or ""):
                target = w
                break
            stack.extend(w.children())
        if target is None:
            from PyQt5.QtCore import QObject
            qw_children = [w.metaObject().className()
                           for w in qw.findChildren(QObject)]
            return {"error": f"canvas widget not found: {qw_children[:12]}"}
        pm = target.grab()
        img = pm.toImage() if hasattr(pm, "toImage") else pm
        from PyQt5.QtCore import QBuffer, QIODevice
        buf = QBuffer()
        buf.open(QIODevice.WriteOnly)
        img.save(buf, "PNG")
        data = bytes(buf.data())
        open(path, "wb").write(data)
        return {"status": "ok", "path": path,
                "width": target.width(), "height": target.height(),
                "widget_class": target.metaObject().className(),
                "device_pixel_ratio": target.devicePixelRatioF()}

    def cmd_set_canvas_lod(self, params):
        """Enable or disable Instant Preview (Level of Detail mode)."""
        enable = bool(params.get("enable", True))
        view = self.get_active_view()
        if not view:
            return {"error": "No active view"}
        canvas = view.canvas()
        if not canvas:
            return {"error": "No active canvas"}
        canvas.setLevelOfDetailMode(enable)
        return {"status": "ok", "instant_preview_lod": canvas.levelOfDetailMode()}


# Register the extension
Krita.instance().addExtension(KritaMCPExtension(Krita.instance()))
