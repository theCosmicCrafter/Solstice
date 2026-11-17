"""JSON-RPC 2.0 front for POST /mcp on the kritamcp listener (Megaplan Phase 2b-2d).

One Streamable-HTTP endpoint on :5678:
  - initialize / ping / tools/list / tools/call; notifications -> 202, batches -> 400
  - tools/call krita_<action>  -> the existing CommandQueue action path (envelope)
  - tools/call <native name>   -> loopback proxy to the C++ TestControl listener at
        127.0.0.1:$SOLSTICE_TEST_CONTROL_PORT/mcp (Authorization rewritten to the
        env token). Env-gated, exactly like the upstream plugin.
  - protocol versions 2025-06-18 / 2025-03-26 / 2024-11-05

`handle` is pure of Krita/Qt state: the caller passes `run_command(command) -> dict`
(the envelope dispatch) so action tools share the window-ready gate and the
_command_processing re-entrancy guard for free.
"""
import json
import os
import sys
import urllib.request
import urllib.error

PROTOCOL_VERSIONS = ("2025-06-18", "2025-03-26", "2024-11-05")
KRITA_PREFIX = "krita_"
PROXY_TIMEOUT_S = 300.0  # run_scenarios.py uses 300; covers wait_idle/realtime strokes

_server_info = {"name": "solstice-kritamcp", "version": "1.0"}
_native_tools_cache = None  # list[dict] | None; invalidated on proxy failure


def _native_endpoint():
    port = os.environ.get("SOLSTICE_TEST_CONTROL_PORT", "").strip()
    if not port or not port.isdigit() or int(port) == 5678:
        return None, None
    token = os.environ.get("SOLSTICE_TEST_CONTROL_TOKEN", "")
    return f"http://127.0.0.1:{port}/mcp", token


def native_tools_available():
    return _native_endpoint()[0] is not None


def _native_rpc(payload, timeout=PROXY_TIMEOUT_S):
    """Forward one JSON-RPC body to the C++ listener; returns parsed JSON or None."""
    url, token = _native_endpoint()
    if url is None:
        return None
    req = urllib.request.Request(
        url, data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json",
                 "Authorization": f"Bearer {token}"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except Exception as exc:
        # urllib computes Host: 127.0.0.1:<port> itself — never set it to a
        # URL-with-path, the native Host check would 403 it.
        print(f"[KritaMCP] native proxy failed: {exc}", file=sys.stderr, flush=True)
        return None


def _native_tools():
    global _native_tools_cache
    if _native_tools_cache is not None:
        return _native_tools_cache
    resp = _native_rpc({"jsonrpc": "2.0", "id": 0, "method": "tools/list"}, timeout=15.0)
    tools = (resp or {}).get("result", {}).get("tools")
    if isinstance(tools, list):
        _native_tools_cache = tools
        return tools
    return []


def _invalidate_native_cache():
    global _native_tools_cache
    _native_tools_cache = None


def _krita_tools(supported_actions):
    return [
        {
            "name": KRITA_PREFIX + action,
            "description": f"Solstice action '{action}' (kritamcp envelope tool).",
            "inputSchema": SCHEMAS.get(action, {"type": "object", "additionalProperties": True}),
        }
        for action in supported_actions
    ]


# Hand-maintained input schemas for the highest-traffic actions; everything
# else advertises a generic object schema until v2 mines handler signatures.
SCHEMAS = {
    "new_canvas": {"type": "object",
                   "properties": {"width": {"type": "integer"}, "height": {"type": "integer"},
                                  "name": {"type": "string"}},
                   "required": ["width", "height"]},
    "set_color": {"type": "object",
                  "properties": {"color": {"type": "array", "items": {"type": "integer"},
                                           "description": "RGBA 0-255"}},
                  "required": ["color"]},
    "stroke": {"type": "object",
               "properties": {"points": {"type": "array",
                                         "items": {"type": "array", "items": {"type": "number"}}},
                              "size": {"type": "number"}},
               "required": ["points"]},
    "fill": {"type": "object", "properties": {}},
    "draw_shape": {"type": "object",
                   "properties": {"shape": {"type": "string", "enum": ["rect", "ellipse", "line"]},
                                  "x": {"type": "number"}, "y": {"type": "number"},
                                  "width": {"type": "number"}, "height": {"type": "number"}},
                   "required": ["shape", "x", "y", "width", "height"]},
    "select_rect": {"type": "object",
                    "properties": {"x": {"type": "number"}, "y": {"type": "number"},
                                   "width": {"type": "number"}, "height": {"type": "number"}},
                    "required": ["x", "y", "width", "height"]},
    "open_file": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]},
    "save": {"type": "object", "properties": {"path": {"type": "string"}}},
    "screenshot": {"type": "object",
                   "properties": {"filename": {"type": "string"}, "zoom": {"type": "number"},
                                  "scope": {"type": "string", "enum": ["window", "canvas"]}}},
    "ai_generate": {"type": "object",
                    "properties": {"mode": {"type": "string",
                                            "enum": ["auto", "fill", "remove", "expand", "create"]},
                                   "prompt": {"type": "string"}, "negative": {"type": "string"},
                                   "style": {"type": "string"}, "strength": {"type": "number"},
                                   "seed": {"type": "integer"}, "count": {"type": "integer"},
                                   "apply": {"type": "boolean"}},
                    "required": ["prompt"]},
    "ai_apply": {"type": "object",
                 "properties": {"job_id": {"type": "string"}, "index": {"type": "integer"},
                                "result": {"type": "integer"}}},
    "ai_jobs": {"type": "object",
                "properties": {"limit": {"type": "integer"}, "state": {"type": "string"}}},
    "export_document": {"type": "object", "properties": {"path": {"type": "string"}},
                        "required": ["path"]},
    "export_layers": {"type": "object", "properties": {"output_dir": {"type": "string"}},
                      "required": ["output_dir"]},
}


def _result(mid, result):
    return {"jsonrpc": "2.0", "id": mid, "result": result}


def _error(mid, code, message):
    return {"jsonrpc": "2.0", "id": mid, "error": {"code": code, "message": message}}


def _tool_result(action_result):
    """Wrap an envelope result dict as an MCP tools/call result."""
    if isinstance(action_result, dict) and "error" in action_result:
        return {"isError": True,
                "content": [{"type": "text", "text": str(action_result["error"])}]}
    return {
        "content": [{"type": "text", "text": json.dumps(action_result, default=str)}],
        "structuredContent": action_result if isinstance(action_result, dict) else {"result": action_result},
    }


def handle(body, *, run_command, supported_actions):
    """Handle one /mcp request body.

    Returns (status_code, response_bytes). 202 notifications return (202, b"").
    `run_command` receives {"action":..., "params":...} and returns the envelope dict.
    """
    try:
        msg = json.loads(body)
    except (json.JSONDecodeError, UnicodeDecodeError):
        return 400, json.dumps(_error(None, -32700, "Parse error")).encode()

    if isinstance(msg, list):
        return 400, json.dumps(_error(None, -32600, "Batches are not supported")).encode()
    if not isinstance(msg, dict) or "method" not in msg:
        return 400, json.dumps(_error(None, -32600, "Invalid Request")).encode()

    mid = msg.get("id")
    method = msg.get("method")
    params = msg.get("params")
    if not isinstance(params, dict):
        params = {}

    if mid is None:
        # notification (initialized, cancelled, ...) -> accept, no reply
        return 202, b""

    if method == "initialize":
        client_ver = params.get("protocolVersion")
        negotiated = client_ver if client_ver in PROTOCOL_VERSIONS else PROTOCOL_VERSIONS[0]
        return 200, json.dumps(_result(mid, {
            "protocolVersion": negotiated,
            "capabilities": {"tools": {"listChanged": False}},
            "serverInfo": _server_info,
        })).encode()

    if method == "ping":
        return 200, json.dumps(_result(mid, {})).encode()

    if method == "tools/list":
        tools = _krita_tools(supported_actions) + _native_tools()
        return 200, json.dumps(_result(mid, {"tools": tools})).encode()

    if method == "tools/call":
        if not isinstance(msg.get("params"), dict):
            return 200, json.dumps(_error(mid, -32602, "params must be an object")).encode()
        name = params.get("name")
        if not isinstance(name, str) or not name:
            return 200, json.dumps(_error(mid, -32602, "params.name is required")).encode()
        args = params.get("arguments") or {}
        if not isinstance(args, dict):
            return 200, json.dumps(_error(mid, -32602, "arguments must be an object")).encode()

        if name.startswith(KRITA_PREFIX):
            action = name[len(KRITA_PREFIX):]
            if action not in supported_actions:
                return 200, json.dumps(_error(mid, -32602, f"Unknown tool: {name}")).encode()
            result = run_command({"action": action, "params": args})
            return 200, json.dumps(_result(mid, _tool_result(result))).encode()

        # native namespace -> proxy verbatim to the C++ listener
        if _native_endpoint()[0] is None:
            return 503, json.dumps(_error(mid, -32000,
                f"Unknown tool: {name} (native tools unavailable: SOLSTICE_TEST_CONTROL_PORT unset)")).encode()
        resp = _native_rpc(msg)
        if resp is None:
            _invalidate_native_cache()
            return 503, json.dumps(_error(mid, -32000,
                "native TestControl listener unreachable")).encode()
        return 200, json.dumps(resp).encode()

    return 200, json.dumps(_error(mid, -32601, f"Method not found: {method}")).encode()


def origin_allowed(origin_header):
    """Foreign-Origin rejection for /mcp (DNS-rebinding guard, upstream parity).

    Absent header -> OK (non-browser clients). Present -> must be loopback.
    """
    if not origin_header or origin_header == "null":
        # absent (non-browser clients) or opaque/sandboxed context — no host
        # to DNS-rebind, nothing to guard against
        return True
    try:
        from urllib.parse import urlparse
        host = urlparse(origin_header).hostname or ""
    except Exception:
        return False
    return host in ("127.0.0.1", "localhost", "::1")


def env_token():
    """SOLSTICE_TEST_CONTROL_TOKEN is a second accepted bearer on :5678."""
    tok = os.environ.get("SOLSTICE_TEST_CONTROL_TOKEN", "")
    return tok if len(tok) >= 16 else ""
