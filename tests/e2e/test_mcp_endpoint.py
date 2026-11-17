# SPDX-License-Identifier: GPL-3.0-or-later
"""Smoke tests for the unified POST /mcp JSON-RPC front (Phase 2b-2d).

Covers the design-doc gate: initialize (all three protocol versions),
tools/list count, a real krita_* tools/call through the GUI command queue,
unknown-tool errors, auth (bad token -> 401), foreign Origin -> 403,
GET /mcp -> 405, notifications -> 202, batches -> 400.

Runs against the same launched/attached instance as the rest of the suite
(`app` fixture from conftest.py); the envelope path and /health are
regression-covered by test_solstice_e2e.py.
"""

import json
import urllib.error
import urllib.request

import pytest

from solstice_bridge import Bridge, BridgeError, DEFAULT_URL, resolve_token  # noqa: E402


def _raw(method, path, body=None, token=None, extra_headers=None, timeout=120):
    """Minimal request that returns (status, parsed-or-None) without client sugar."""
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    if extra_headers:
        headers.update(extra_headers)
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(DEFAULT_URL + path, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read()
            return resp.status, (json.loads(raw) if raw else None)
    except urllib.error.HTTPError as e:
        raw = e.read()
        try:
            return e.code, (json.loads(raw) if raw else None)
        except Exception:
            return e.code, None
    except urllib.error.URLError as e:
        raise BridgeError(f"cannot reach {DEFAULT_URL}: {e}") from e


def _rpc(method, params=None, rid=1, **kwargs):
    body = {"jsonrpc": "2.0", "id": rid, "method": method}
    if params is not None:
        body["params"] = params
    return _raw("POST", "/mcp", body, token=resolve_token(), **kwargs)


def test_mcp_initialize_all_versions(app):
    for ver in ("2025-06-18", "2025-03-26", "2024-11-05"):
        status, r = _rpc("initialize", {"protocolVersion": ver,
                                      "capabilities": {}, "clientInfo": {"name": "e2e"}})
        assert status == 200, r
        assert r["result"]["protocolVersion"] == ver, r
        assert r["result"]["serverInfo"]["name"].startswith("solstice"), r
        assert "tools" in r["result"]["capabilities"], r


def test_mcp_ping(app):
    status, r = _rpc("ping")
    assert status == 200 and r["result"] == {}, r


def test_mcp_tools_list(app):
    status, r = _rpc("tools/list")
    assert status == 200, r
    tools = r["result"]["tools"]
    names = {t["name"] for t in tools}
    # every envelope action is exposed as krita_<action>
    for action in ("new_canvas", "ai_status", "list_actions", "stroke", "save"):
        assert f"krita_{action}" in names, names
    krita_tools = [t for t in tools if t["name"].startswith("krita_")]
    assert len(krita_tools) >= 80, len(krita_tools)
    for t in krita_tools:
        assert t["inputSchema"]["type"] == "object"


def test_mcp_call_krita_ai_status(app):
    status, r = _rpc("tools/call", {"name": "krita_ai_status", "arguments": {}})
    assert status == 200, r
    result = r["result"]
    assert result.get("isError") is not True, result
    assert "structuredContent" in result
    assert result["content"][0]["type"] == "text"


def test_mcp_call_krita_new_canvas(app):
    status, r = _rpc("tools/call", {
        "name": "krita_new_canvas",
        "arguments": {"width": 64, "height": 64, "name": "mcp-smoke"},
    })
    assert status == 200, r
    result = r["result"]
    assert result.get("isError") is not True, result


def test_mcp_unknown_tool(app):
    status, r = _rpc("tools/call", {"name": "krita_does_not_exist", "arguments": {}})
    assert status == 200, r
    assert r["error"]["code"] == -32602


def test_mcp_unknown_method(app):
    status, r = _rpc("resources/list")
    assert status == 200 and r["error"]["code"] == -32601, r


def test_mcp_bad_token_401(app):
    status, _ = _raw("POST", "/mcp", {"jsonrpc": "2.0", "id": 1, "method": "ping"},
                     token="0" * 48)
    assert status == 401


def test_mcp_foreign_origin_403(app):
    status, _ = _raw("POST", "/mcp", {"jsonrpc": "2.0", "id": 1, "method": "ping"},
                     token=resolve_token(),
                     extra_headers={"Origin": "https://evil.example.com"})
    assert status == 403


def test_mcp_loopback_origin_ok(app):
    status, _ = _raw("POST", "/mcp", {"jsonrpc": "2.0", "id": 1, "method": "ping"},
                     token=resolve_token(),
                     extra_headers={"Origin": "http://localhost:5678"})
    assert status == 200


def test_mcp_get_405(app):
    status, _ = _raw("GET", "/mcp", token=resolve_token())
    assert status == 405


def test_mcp_notification_202(app):
    status, payload = _raw("POST", "/mcp",
                           {"jsonrpc": "2.0", "method": "notifications/initialized"},
                           token=resolve_token())
    assert status == 202 and payload is None


def test_mcp_batch_400(app):
    status, _ = _raw("POST", "/mcp",
                     [{"jsonrpc": "2.0", "id": 1, "method": "ping"}],
                     token=resolve_token())
    assert status == 400


def test_mcp_env_token_accepted(app, monkeypatch):
    # Only meaningful when the runner exports SOLSTICE_TEST_CONTROL_TOKEN;
    # documents the second-bearer contract when the env is present.
    import os
    env_tok = os.environ.get("SOLSTICE_TEST_CONTROL_TOKEN", "")
    if len(env_tok) < 16:
        pytest.skip("SOLSTICE_TEST_CONTROL_TOKEN not set")
    status, r = _raw("POST", "/mcp", {"jsonrpc": "2.0", "id": 1, "method": "ping"},
                     token=env_tok)
    assert status == 200 and r["result"] == {}
