#!/usr/bin/env python3
#
#  SPDX-License-Identifier: GPL-3.0-or-later
#
"""Tests for run_scenarios.py against a fake test control endpoint.

Run: python dev-tools/test-control/test_run_scenarios.py (registered with
ctest as plugins-extensions-testcontrol-RunScenariosTest).
"""

import base64
import contextlib
import io
import json
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_scenarios  # noqa: E402

TOKEN = "0123456789abcdef0123"
PNG = b"\x89PNG\r\n\x1a\nfake image bytes"


class FakeEndpoint:
    """Answers the tools the tests use; records every call."""

    def __init__(self):
        self.calls = []
        self.counter = 0
        self.value = 7

    def tool(self, name, arguments):
        self.calls.append((name, arguments))
        if name == "session_info":
            return False, {"mainWindows": 1, "environment": {}, "solsticeVersion": "test", "platform": "fake"}, []
        if name == "document_close":
            return False, {"closed": 0}, []
        if name == "echo":
            return False, dict(arguments), []
        if name == "count":
            self.counter += 1
            return False, {"count": self.counter, "nested": {"list": [10, 20]}}, []
        if name == "value":
            return False, {"value": self.value}, []
        if name == "broken":
            return True, {"error": "it broke"}, []
        if name == "pixels":
            result = {"sha256": "abc"}
            images = []
            if arguments.get("fullResolutionPng"):
                images.append({"type": "image", "mimeType": "image/png", "data": base64.b64encode(PNG).decode()})
            if "reference" in arguments:
                result["reference"] = {"matches": base64.b64decode(arguments["reference"]) == PNG}
            return False, result, images
        raise KeyError(name)


def serve(endpoint):
    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):
            if self.headers.get("Authorization") != "Bearer " + TOKEN:
                self.send_response(401)
                self.end_headers()
                return
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            if body["method"] == "tools/call":
                is_error, structured, images = endpoint.tool(body["params"]["name"], body["params"]["arguments"])
                result = {"isError": is_error, "structuredContent": structured,
                          "content": [{"type": "text", "text": json.dumps(structured)}] + images}
            else:
                result = {}
            payload = json.dumps({"jsonrpc": "2.0", "id": body["id"], "result": result}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def log_message(self, *args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


class RunnerTest(unittest.TestCase):
    def setUp(self):
        self.endpoint = FakeEndpoint()
        self.server = serve(self.endpoint)
        self.directory = Path(tempfile.mkdtemp())

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()

    def scenario(self, steps, name="sample"):
        path = self.directory / (name + ".json")
        path.write_text(json.dumps({"format": run_scenarios.SCENARIO_FORMAT, "version": 1,
                                    "name": name, "steps": steps}))
        return path

    def run_runner(self, *arguments):
        argv = [str(a) for a in arguments] + ["--port", str(self.server.server_address[1]), "--token", TOKEN]
        output = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            status = run_scenarios.main(argv)
        return status, output.getvalue()

    def report(self, *arguments):
        report_path = self.directory / "report.json"
        status, output = self.run_runner(*arguments, "--report", report_path)
        return status, json.loads(report_path.read_text()) if report_path.exists() else None, output

    def test_matchers_and_saved_values(self):
        path = self.scenario([
            {"call": "count", "save": {"first": "count"}, "expect": {"count": 1, "nested.list.1": 20}},
            {"call": "count", "expect": {"count": {"min": 2, "max": 2, "notEquals": "$first"}}},
            {"call": "echo", "args": {"previous": "$first"}, "expect": {"previous": {"equals": "$first"}}},
        ])
        status, report, _ = self.report(path)
        self.assertEqual(status, 0)
        self.assertEqual(report["format"], run_scenarios.REPORT_FORMAT)
        self.assertEqual(report["scenarios"][0]["status"], "pass")
        self.assertEqual([s["status"] for s in report["scenarios"][0]["steps"]], ["pass"] * 3)
        self.assertEqual(self.endpoint.calls[-2], ("echo", {"previous": 1}))
        # Each scenario starts and ends without documents.
        self.assertEqual(self.endpoint.calls[1], ("document_close", {"all": True}))
        self.assertEqual(self.endpoint.calls[-1], ("document_close", {"all": True}))

    def test_failures_are_reported(self):
        path = self.scenario([
            {"call": "count", "expect": {"count": 2, "missing.path": 1}},
            {"call": "count", "expect": {"count": {"max": 1}}},
        ])
        status, report, output = self.report(path)
        self.assertEqual(status, 1)
        steps = report["scenarios"][0]["steps"]
        self.assertEqual([s["status"] for s in steps], ["fail", "fail"])
        self.assertIn("count: expected 2, got 1", steps[0]["problems"])
        self.assertIn("missing.path: expected 1, got missing", steps[0]["problems"])
        self.assertIn("expected max 1", steps[1]["problems"][0])
        self.assertIn("FAIL", output)

    def test_fail_fast_and_tool_errors(self):
        path = self.scenario([{"call": "count", "expect": {"count": 5}}, {"call": "count"}])
        status, report, _ = self.report(path, "--fail-fast")
        self.assertEqual(status, 1)
        self.assertEqual(len(report["scenarios"][0]["steps"]), 1)

        path = self.scenario([{"call": "broken"}, {"call": "count"}], name="broken")
        status, report, _ = self.report(path)
        self.assertEqual(status, 1)
        steps = report["scenarios"][0]["steps"]
        self.assertEqual(len(steps), 1)
        self.assertEqual(steps[0]["status"], "error")
        self.assertIn("it broke", steps[0]["problems"][0])

    def test_file_substitution(self):
        (self.directory / "data.bin").write_bytes(b"\x00\x01payload")
        path = self.scenario([{"call": "echo", "args": {"blob": "@data.bin"},
                               "expect": {"blob": base64.b64encode(b"\x00\x01payload").decode()}}])
        status, _, _ = self.report(path)
        self.assertEqual(status, 0)

    def test_recorded_values(self):
        path = self.scenario([{"call": "value", "expect": {"value": {"recorded": "theValue"}}}])
        expected_file = self.directory / "sample.expected.json"

        status, report, _ = self.report(path)
        self.assertEqual(status, 0)
        self.assertEqual(report["scenarios"][0]["status"], "unrecorded")
        status, _, _ = self.report(path, "--strict")
        self.assertEqual(status, 1)
        self.assertFalse(expected_file.exists())

        status, _, _ = self.report(path, "--record")
        self.assertEqual(status, 0)
        stored = json.loads(expected_file.read_text())
        self.assertEqual(stored["format"], run_scenarios.EXPECTED_FORMAT)
        key = report["platformKey"]
        self.assertEqual(stored["values"]["theValue"][key], 7)

        status, report, _ = self.report(path, "--strict")
        self.assertEqual(status, 0)
        self.assertEqual(report["scenarios"][0]["status"], "pass")

        self.endpoint.value = 8
        status, report, _ = self.report(path)
        self.assertEqual(status, 1)
        self.assertIn("expected recorded theValue 7, got 8", report["scenarios"][0]["steps"][0]["problems"][0])

    def test_reference_images(self):
        path = self.scenario([{"call": "pixels", "args": {"reference": "@ref:stroke"},
                               "expect": {"reference.matches": True}}])
        status, report, _ = self.report(path)
        self.assertEqual(status, 0)
        self.assertEqual(report["scenarios"][0]["steps"][0]["status"], "unrecorded")
        self.assertNotIn("reference", self.endpoint.calls[-2][1])

        status, report, _ = self.report(path, "--record")
        self.assertEqual(status, 0)
        self.assertTrue(self.endpoint.calls[-2][1]["fullResolutionPng"])
        reference = self.directory / "sample.refs" / report["platformKey"] / "stroke.png"
        self.assertEqual(reference.read_bytes(), PNG)

        status, report, _ = self.report(path, "--strict")
        self.assertEqual(status, 0)
        self.assertEqual(base64.b64decode(self.endpoint.calls[-2][1]["reference"]), PNG)

    def test_discovery_and_errors(self):
        root = self.directory / "repo"
        (root / "a" / "tests" / "scenarios").mkdir(parents=True)
        (root / "_build" / "x" / "tests" / "scenarios").mkdir(parents=True)
        for path in (root / "a/tests/scenarios/one.json", root / "a/tests/scenarios/one.expected.json",
                     root / "_build/x/tests/scenarios/two.json"):
            path.write_text("{}")
        self.assertEqual(run_scenarios.discover(root), [root / "a/tests/scenarios/one.json"])

        bad = self.directory / "bad.json"
        bad.write_text(json.dumps({"format": "other", "version": 1}))
        status, output = self.run_runner(bad)
        self.assertEqual(status, 2)
        self.assertIn("not a solstice-test-control-scenario", output)

        # Nothing listens on port 1.
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            status = run_scenarios.main([str(self.scenario([{"call": "count"}])), "--port", "1", "--token", TOKEN])
        self.assertEqual(status, 2)


if __name__ == "__main__":
    unittest.main()
