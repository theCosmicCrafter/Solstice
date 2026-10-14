# SPDX-License-Identifier: GPL-3.0-or-later
"""Minimal client for the vendored kritamcp bridge (127.0.0.1:5678).

Used by the E2E suite to drive a real Solstice instance: documents, layers,
pixels, selections, filters, save/export, and auth enforcement.

Token resolution mirrors the external MCP server:
  KRITAMCP_TOKEN env var -> [kritamcp] token in the Solstice kritarc.
"""

import json
import os
import urllib.error
import urllib.request
from pathlib import Path

DEFAULT_URL = os.environ.get("KRITA_URL", "http://127.0.0.1:5678")


def kritarc_path():
    appdata = os.environ.get("APPDATA", str(Path.home() / "AppData" / "Roaming"))
    return Path(appdata) / "Solstice" / "config" / "kritarc"


def resolve_token():
    token = os.environ.get("KRITAMCP_TOKEN", "")
    if token:
        return token
    # Sidecar files, written at generation time (kritarc syncs on app exit).
    # The plugin writes it under QStandardPaths.AppConfigLocation, which is
    # %LOCALAPPDATA%\krita because Solstice keeps the Qt app name "krita".
    appdata = Path(os.environ.get("APPDATA", ""))
    localdata = Path(os.environ.get("LOCALAPPDATA", ""))
    for sidecar in (
        localdata / "krita" / "kritamcp.token",
        appdata / "krita" / "kritamcp.token",
        kritarc_path().parent / "kritamcp.token",
    ):
        try:
            token = sidecar.read_text(encoding="ascii").strip()
            if token:
                return token
        except OSError:
            pass
    # kritarc is a KDE config: keys can appear before any [section], so
    # configparser cannot parse it. Scan for the [kritamcp] group by hand.
    rc = kritarc_path()
    try:
        in_group = False
        for line in rc.read_text(encoding="utf-8", errors="replace").splitlines():
            line = line.strip()
            if line.startswith("["):
                in_group = line.strip("[]").strip() == "kritamcp"
                continue
            if in_group and line.startswith("token="):
                return line.split("=", 1)[1].strip()
    except OSError:
        pass
    return ""


class BridgeError(Exception):
    pass


class Bridge:
    def __init__(self, url=DEFAULT_URL, token=None):
        self.url = url.rstrip("/")
        self._token = token if token is not None else resolve_token()

    def _request(self, method, path, body=None, token=None):
        headers = {"Content-Type": "application/json"}
        if token:
            headers["Authorization"] = f"Bearer {token}"
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(self.url + path, data=data, headers=headers, method=method)
        try:
            with urllib.request.urlopen(req, timeout=120) as resp:
                return resp.status, json.loads(resp.read() or b"{}")
        except urllib.error.HTTPError as e:
            payload = {}
            try:
                payload = json.loads(e.read() or b"{}")
            except Exception:
                pass
            return e.code, payload
        except urllib.error.URLError as e:
            raise BridgeError(f"cannot reach {self.url}: {e}") from e

    def health(self):
        return self._request("GET", "/health")

    def cmd(self, action, params=None, token="__default__"):
        if token == "__default__":
            if not self._token:
                # The server generates the token lazily on its first request;
                # re-resolve so a session-scoped client picks it up mid-run.
                self._token = resolve_token()
            tok = self._token
        else:
            tok = token
        status, payload = self._request("POST", "/", {"action": action, "params": params or {}}, tok)
        return status, payload

    def cmd_ok(self, action, params=None):
        status, payload = self.cmd(action, params)
        if status != 200 or "error" in payload:
            raise BridgeError(f"{action} failed ({status}): {payload}")
        return payload
