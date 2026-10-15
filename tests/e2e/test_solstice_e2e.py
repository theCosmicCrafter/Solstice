# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end tests driving a real solstice.exe through the vendored
kritamcp bridge. No krita stubs — every assertion observes the running app
(pixel data, files on disk, HTTP responses, process state).

Covered: bridge health, bearer auth, document lifecycle, real pixel writes
and readback, stroke rendering, layer ops (incl. undo/redo on node ops —
pixel writes are not undoable in Krita), selections, filters, save/export
file validation (magic bytes + PNG dims), path-traversal rejection,
unknown-action handling, and a soak pass that watches process health.
"""

import struct
import sys
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent))
from solstice_bridge import resolve_token  # noqa: E402


def _png_dims(path):
    data = Path(path).read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    w, h = struct.unpack(">II", data[16:24])
    return w, h


def _layer_names(layers):
    return [l["name"] for l in layers]


# ─── connectivity & auth ────────────────────────────────────────────

def test_health(app):
    bridge, _ = app
    status, payload = bridge.health()
    assert status == 200 and payload.get("status") == "ok"


def test_auth_enforcement(app):
    bridge, _ = app
    # Missing token -> rejected
    status, payload = bridge.cmd("list_layers", token="")
    assert status == 401 or "error" in payload
    # Wrong token -> rejected
    status, payload = bridge.cmd("list_layers", token="wrong-token")
    assert status == 401 or "error" in payload
    # The rejected probes above made the server generate+persist its token.
    token = resolve_token()
    assert token, "no kritamcp token found (kritarc or kritamcp.token sidecar)"
    # list_brushes needs no open document — pure auth check.
    status, payload = bridge.cmd("list_brushes", token=token)
    assert status == 200 and "error" not in payload


def test_unknown_action(app):
    bridge, _ = app
    status, payload = bridge.cmd("definitely_not_an_action")
    assert "error" in payload and "Unknown action" in payload["error"]


# ─── document lifecycle ─────────────────────────────────────────────

def test_document_lifecycle(app, tmp_path):
    bridge, _ = app
    bridge.cmd_ok("new_canvas", {"width": 320, "height": 240, "name": "e2e_doc"})
    info = bridge.cmd_ok("get_document_info")
    assert info["width"] == 320 and info["height"] == 240
    assert info["color_model"] == "RGBA" and info["color_depth"] == "U8"

    out = tmp_path / "e2e_doc.kra"
    bridge.cmd_ok("save", {"path": str(out)})
    # .kra is a ZIP container — verify real bytes, not just existence,
    # because cmd_save ignores exportImage()'s return value.
    assert out.exists() and out.stat().st_size > 1024
    assert out.read_bytes()[:4] == b"PK\x03\x04", "saved .kra is not a ZIP container"

    reopened = bridge.cmd_ok("open_file", {"path": str(out)})
    assert reopened["width"] == 320 and reopened["height"] == 240


# ─── pixels ─────────────────────────────────────────────────────────

def test_paint_and_pixels(app):
    bridge, _ = app
    bridge.cmd_ok("new_canvas", {"width": 200, "height": 200, "background": "#112233"})
    bridge.cmd_ok("set_color", {"color": "#ff0000"})
    bridge.cmd_ok("fill", {"x": 60, "y": 60, "radius": 15})

    inside = bridge.cmd_ok("get_color_at", {"x": 60, "y": 60})
    assert inside["r"] > 200 and inside["g"] < 80 and inside["b"] < 80

    outside = bridge.cmd_ok("get_color_at", {"x": 180, "y": 180})
    assert abs(outside["r"] - 0x11) < 20 and abs(outside["b"] - 0x33) < 20


def test_stroke_writes_pixels(app):
    bridge, _ = app
    bridge.cmd_ok("new_canvas", {"width": 200, "height": 200, "background": "#000000"})
    bridge.cmd_ok("set_color", {"color": "#00ff00"})
    bridge.cmd_ok("stroke", {
        "points": [[20, 100], [180, 100]],
        "size": 10,
        "hardness": 1.0,
    })
    px = bridge.cmd_ok("get_color_at", {"x": 100, "y": 100})
    assert px["g"] > 200, f"stroke did not land on the pixel path: {px}"
    off = bridge.cmd_ok("get_color_at", {"x": 100, "y": 20})
    assert off["g"] < 60, f"stroke bled outside its path: {off}"


# ─── layers & undo ─────────────────────────────────────────────────

def test_layer_ops(app):
    bridge, _ = app
    bridge.cmd_ok("new_canvas", {"width": 100, "height": 100})
    bridge.cmd_ok("add_layer", {"name": "e2e_layer"})
    layers = bridge.cmd_ok("list_layers")["layers"]
    assert "e2e_layer" in _layer_names(layers)

    bridge.cmd_ok("set_layer_opacity", {"name": "e2e_layer", "opacity": 128})
    layers = bridge.cmd_ok("list_layers")["layers"]
    mine = next(l for l in layers if l["name"] == "e2e_layer")
    assert mine["opacity"] == 128

    bridge.cmd_ok("set_layer_visibility", {"name": "e2e_layer", "visible": False})
    layers = bridge.cmd_ok("list_layers")["layers"]
    mine = next(l for l in layers if l["name"] == "e2e_layer")
    assert mine["visible"] is False

    # duplicate_layer names the clone "<name> Copy"
    dup = bridge.cmd_ok("duplicate_layer", {"name": "e2e_layer"})
    assert dup["copy"] == "e2e_layer Copy"
    bridge.cmd_ok("remove_layer", {"name": "e2e_layer Copy"})
    bridge.cmd_ok("remove_layer", {"name": "e2e_layer"})
    layers = bridge.cmd_ok("list_layers")["layers"]
    assert "e2e_layer" not in _layer_names(layers)
    assert "e2e_layer Copy" not in _layer_names(layers)


def _wait_for(predicate, timeout=10.0, interval=0.25):
    """Poll until predicate() is true — bridge commands like undo trigger
    Krita actions whose effects land asynchronously on the stroke queue."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(interval)
    return False


def test_trigger_action_and_trace_to_vector(app):
    """trigger_action drives application actions end-to-end. Uses
    layer_to_vector (Solstice Trace-to-Vector) as the deep check: paint
    pixels, trigger the action, then the layer list must gain a
    vectorlayer node. Effects land asynchronously — poll."""
    bridge, _ = app
    bridge.cmd_ok("new_canvas", {"width": 100, "height": 100})
    bridge.cmd_ok("fill", {"x": 50, "y": 50, "radius": 20})

    status, payload = bridge.cmd("trigger_action", {"name": "layer_to_vector"})
    if status == 200 and "Unknown action" in payload.get("error", ""):
        pytest.skip("layer_to_vector not in this build")

    # The action's enabled state syncs on idle — retry the trigger
    # until the traced layer actually appears (same pattern as undo).
    deadline = time.time() + 15
    traced = None
    while time.time() < deadline and traced is None:
        bridge.cmd_ok("trigger_action", {"name": "layer_to_vector"})
        time.sleep(0.5)
        layers = bridge.cmd_ok("list_layers")["layers"]
        for layer in layers:
            if layer["type"] == "vectorlayer":
                traced = layer
                break
    assert traced, "trigger_action(layer_to_vector) produced no vector layer"
    assert "Traced" in traced["name"]

    status, payload = bridge.cmd("trigger_action", {"name": "no_such_action_xyz"})
    assert status == 200 and "error" in payload


def test_undo_redo_layer_ops(app):
    """Node add/remove is undoable; pixel writes are not — so undo is
    verified on layer operations where Krita has a real undo adapter.
    Undo/redo effects land asynchronously — poll for them."""
    bridge, _ = app
    bridge.cmd_ok("new_canvas", {"width": 100, "height": 100})
    bridge.cmd_ok("add_layer", {"name": "undo_target"})

    # The edit_undo action's enabled state syncs on idle — retry the
    # trigger until the undo actually lands.
    deadline = time.time() + 15
    gone = False
    while time.time() < deadline and not gone:
        bridge.cmd_ok("undo")
        time.sleep(0.5)
        gone = "undo_target" not in _layer_names(bridge.cmd_ok("list_layers")["layers"])
    assert gone, "undo did not remove the added layer"

    deadline = time.time() + 15
    back = False
    while time.time() < deadline and not back:
        bridge.cmd_ok("redo")
        time.sleep(0.5)
        back = "undo_target" in _layer_names(bridge.cmd_ok("list_layers")["layers"])
    assert back, "redo did not restore the layer"


def test_merge_and_flatten(app):
    bridge, _ = app
    bridge.cmd_ok("new_canvas", {"width": 80, "height": 80})
    bridge.cmd_ok("add_layer", {"name": "top"})
    bridge.cmd_ok("set_active_layer", {"name": "top"})
    before = bridge.cmd_ok("list_layers")["count"]
    bridge.cmd_ok("merge_down")
    after = bridge.cmd_ok("list_layers")["count"]
    assert after == before - 1
    bridge.cmd_ok("flatten_image")
    info = bridge.cmd_ok("get_document_info")
    assert info["layer_count"] == 1


# ─── selections & filters ───────────────────────────────────────────

def test_selection_ops(app):
    bridge, _ = app
    bridge.cmd_ok("new_canvas", {"width": 100, "height": 100})
    bridge.cmd_ok("select_rect", {"x": 10, "y": 10, "width": 40, "height": 40})
    bridge.cmd_ok("deselect")
    bridge.cmd_ok("select_all")
    bridge.cmd_ok("deselect")


def test_filters(app):
    bridge, _ = app
    filters = bridge.cmd_ok("list_filters")["filters"]
    assert len(filters) > 20
    blur = next((f for f in filters if "blur" in f.lower()), None)
    assert blur, "no blur filter found"

    bridge.cmd_ok("new_canvas", {"width": 120, "height": 120})
    bridge.cmd_ok("set_color", {"color": "#ffffff"})
    bridge.cmd_ok("fill", {"x": 60, "y": 60, "radius": 30})
    res = bridge.cmd_ok("apply_filter", {"name": blur})
    assert res["filter"] == blur


# ─── save / export ──────────────────────────────────────────────────

def test_export_png(app, tmp_path):
    bridge, _ = app
    bridge.cmd_ok("new_canvas", {"width": 96, "height": 64, "background": "#224466"})
    out = tmp_path / "e2e_export.png"
    res = bridge.cmd_ok("export_document", {"path": str(out)})
    assert res.get("success") is True
    assert _png_dims(out) == (96, 64)


def test_path_traversal_rejected(app):
    bridge, _ = app
    bridge.cmd_ok("new_canvas", {"width": 50, "height": 50})
    status, payload = bridge.cmd("save", {"path": "..\\..\\evil.kra"})
    assert "error" in payload and "traversal" in payload["error"].lower()
    status, payload = bridge.cmd("open_file", {"path": "..\\..\\secret.kra"})
    assert "error" in payload


def test_error_paths(app):
    """Malformed/impossible params get clean errors, and the bridge keeps
    answering afterwards."""
    bridge, _ = app
    status, payload = bridge.cmd("get_color_at", {"x": -99999, "y": -99999})
    # Implementation clamps into the projection; either a result or a
    # clean error is acceptable — but it must not hang or crash.
    assert status == 200
    status, payload = bridge.cmd("remove_layer", {"name": "no_such_layer_xyz"})
    assert "error" in payload
    status, payload = bridge.cmd("set_brush", {"preset": "definitely_not_a_brush_xyz"})
    assert "error" in payload
    status, payload = bridge.cmd("stroke", {"points": [[1, 1]]})
    assert "error" in payload
    # Bridge still alive after the bad inputs.
    status, payload = bridge.health()
    assert status == 200


# ─── resize / resolution ────────────────────────────────────────────

def test_resize_scale_resolution(app):
    bridge, _ = app
    bridge.cmd_ok("new_canvas", {"width": 100, "height": 100})
    bridge.cmd_ok("resize_canvas", {"width": 150, "height": 120})
    info = bridge.cmd_ok("get_document_info")
    assert info["width"] == 150 and info["height"] == 120
    bridge.cmd_ok("scale_image", {"width": 75, "height": 60})
    info = bridge.cmd_ok("get_document_info")
    assert info["width"] == 75 and info["height"] == 60
    bridge.cmd_ok("set_resolution", {"dpi": 72})
    info = bridge.cmd_ok("get_document_info")
    assert abs(info["resolution"] - 72) < 0.5


# ─── stability ──────────────────────────────────────────────────────

def test_repeated_ops_stability(app):
    """Rapid-fire mixed operations on one document; the app must stay
    responsive and pixel-consistent throughout."""
    bridge, managed = app
    bridge.cmd_ok("new_canvas", {"width": 120, "height": 120, "background": "#101010"})
    for i in range(10):
        bridge.cmd_ok("set_color", {"color": "#ff0000" if i % 2 == 0 else "#0000ff"})
        bridge.cmd_ok("fill", {"x": 60, "y": 60, "radius": 10})
        px = bridge.cmd_ok("get_color_at", {"x": 60, "y": 60})
        expected = "r" if i % 2 == 0 else "b"
        assert px[expected] > 200, f"iteration {i}: fill color not applied: {px}"
    if managed.proc:
        assert managed.proc.poll() is None, "process died during repeated ops"


@pytest.mark.slow
@pytest.mark.soak
def test_soak(app, tmp_path):
    """Full lifecycle loop: create → paint → save → reopen, N times.
    Catches slow leaks/hangs that single-shot tests miss."""
    import os
    iterations = int(os.environ.get("SOLSTICE_SOAK_ITERS", "5"))
    bridge, managed = app
    for i in range(iterations):
        bridge.cmd_ok("new_canvas", {"width": 200, "height": 200, "name": f"soak_{i}"})
        bridge.cmd_ok("set_color", {"color": "#00ff88"})
        bridge.cmd_ok("fill", {"x": 100, "y": 100, "radius": 40})
        px = bridge.cmd_ok("get_color_at", {"x": 100, "y": 100})
        assert px["g"] > 200, f"soak {i}: fill did not apply"
        out = tmp_path / f"soak_{i}.kra"
        bridge.cmd_ok("save", {"path": str(out)})
        assert out.stat().st_size > 0 and out.read_bytes()[:4] == b"PK\x03\x04"
        if managed.proc:
            assert managed.proc.poll() is None, f"process died at soak iteration {i}"
        time.sleep(1)
    # Reopen the last saved file — proves save integrity across the run.
    reopened = bridge.cmd_ok("open_file", {"path": str(out)})
    assert reopened["width"] == 200


def test_plugin_load_clean(app):
    """No Python tracebacks or plugin load failures in app stderr."""
    _, managed = app
    if not managed.proc:
        pytest.skip("attach mode — stderr not captured")
    time.sleep(2)  # let late plugin imports flush
    text = b"".join(managed.stderr_lines).decode("utf-8", "replace")
    assert "Traceback" not in text, f"plugin traceback in stderr:\n{text[-3000:]}"
