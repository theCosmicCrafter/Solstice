#!/usr/bin/env python3
#
#  SPDX-License-Identifier: GPL-3.0-or-later
#
"""Replay test control scenarios against a running Solstice, without an LLM.

Scenarios are JSON files next to the covered feature's tests
(``*/tests/scenarios/*.json``, format "solstice-test-control-scenario",
version 1). Each step calls one test control tool through the MCP endpoint and
checks its result. See docs/agent/test-control.md ("Scenarios" and "Scenario
runner").

Usage (from any directory; Python 3.8+, standard library only):
    python dev-tools/test-control/run_scenarios.py [SCENARIO ...] [--all]
        [--port 8765] [--token TOKEN] [--report report.json] [--record]
        [--strict] [--fail-fast] [--launch PATH-TO-SOLSTICE] [--list]

Without --launch, Solstice must already run with test control enabled; the
port and token default to SOLSTICE_TEST_CONTROL_PORT and
SOLSTICE_TEST_CONTROL_TOKEN. With --launch, the runner starts that executable
with a scratch profile, test root, port and token, and quits it at the end.

Scenario expectations compare English UI texts (undo command names, button
labels). --launch therefore sets the scratch profile's UI language to English
(--language); against a running Solstice in another language the runner
warns, and text expectations fail.

Exit status: 0 when every scenario passed, 1 when one failed, 2 on a usage,
scenario-file or connection error.
"""

import argparse
import base64
import json
import os
import platform
import secrets
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCENARIO_FORMAT = "solstice-test-control-scenario"
EXPECTED_FORMAT = "solstice-test-control-expected"
REPORT_FORMAT = "solstice-test-control-report"
MATCHER_KEYS = {"equals", "notEquals", "min", "max", "recorded"}
REFERENCE_PREFIX = "@ref:"
MISSING = object()


class RunnerError(Exception):
    """A problem that stops the run (usage, scenario file, connection)."""


class Client:
    """Minimal MCP Streamable HTTP client (JSON responses only)."""

    def __init__(self, url, token, timeout=300):
        self.url = url
        self.token = token
        self.timeout = timeout
        self.next_id = 0

    def request(self, method, params=None):
        self.next_id += 1
        body = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            body["params"] = params
        request = urllib.request.Request(
            self.url,
            data=json.dumps(body).encode(),
            method="POST",
            headers={
                "Content-Type": "application/json",
                "Accept": "application/json, text/event-stream",
                "Authorization": "Bearer " + self.token,
                "MCP-Protocol-Version": "2025-06-18",
            },
        )
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                reply = json.loads(response.read() or b"null")
        except urllib.error.HTTPError as error:
            raise RunnerError(f"{self.url}: HTTP {error.code} for {method}") from error
        except (urllib.error.URLError, OSError) as error:
            raise RunnerError(f"{self.url}: {error}") from error
        if "error" in reply:
            raise RunnerError(f"{method}: {reply['error'].get('message', reply['error'])}")
        return reply["result"]

    def call(self, name, arguments):
        """Returns (is_error, structured result, image blocks)."""
        result = self.request("tools/call", {"name": name, "arguments": arguments})
        images = [block for block in result.get("content", []) if block.get("type") == "image"]
        return bool(result.get("isError")), result.get("structuredContent") or {}, images


# --- Scenario files --------------------------------------------------------


def load_scenario(path):
    try:
        scenario = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise RunnerError(f"{path}: {error}") from error
    if scenario.get("format") != SCENARIO_FORMAT or scenario.get("version") != 1:
        raise RunnerError(f"{path}: not a {SCENARIO_FORMAT} file of version 1")
    if not isinstance(scenario.get("steps"), list) or not scenario.get("name"):
        raise RunnerError(f"{path}: needs a name and a list of steps")
    return scenario


def discover(root):
    """Every scenario file under root/**/tests/scenarios/, sorted."""
    found = []
    for path in sorted(Path(root).glob("**/tests/scenarios/*.json")):
        if path.name.endswith(".expected.json") or "_build" in path.parts:
            continue
        found.append(path)
    return found


class Expected:
    """Recorded values of one scenario, per platform key (<name>.expected.json)."""

    def __init__(self, scenario_path, scenario_name):
        self.path = Path(scenario_path).with_name(scenario_name + ".expected.json")
        self.values = {}
        self.changed = False
        if self.path.exists():
            data = json.loads(self.path.read_text(encoding="utf-8"))
            if data.get("format") != EXPECTED_FORMAT or data.get("version") != 1:
                raise RunnerError(f"{self.path}: not a {EXPECTED_FORMAT} file of version 1")
            self.values = data.get("values", {})

    def get(self, key, platform_key):
        return self.values.get(key, {}).get(platform_key, MISSING)

    def put(self, key, platform_key, value):
        if self.values.get(key, {}).get(platform_key, MISSING) != value:
            self.values.setdefault(key, {})[platform_key] = value
            self.changed = True

    def save(self):
        if not self.changed:
            return
        data = {"format": EXPECTED_FORMAT, "version": 1, "values": self.values}
        self.path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def reference_path(scenario_path, scenario_name, platform_key, name):
    return Path(scenario_path).parent / (scenario_name + ".refs") / platform_key / (name + ".png")


# --- Values and matchers ----------------------------------------------------


def lookup(data, dotted):
    for part in dotted.split("."):
        if isinstance(data, dict) and part in data:
            data = data[part]
        elif isinstance(data, list) and part.isdigit() and int(part) < len(data):
            data = data[int(part)]
        else:
            return MISSING
    return data


def substitute(value, saved, base_dir):
    """Replaces "$name" with a saved value and "@path" with the file's base64."""
    if isinstance(value, dict):
        return {key: substitute(item, saved, base_dir) for key, item in value.items()}
    if isinstance(value, list):
        return [substitute(item, saved, base_dir) for item in value]
    if isinstance(value, str) and value.startswith("$"):
        if value[1:] not in saved:
            raise RunnerError(f"no saved value {value}")
        return saved[value[1:]]
    if isinstance(value, str) and value.startswith("@") and not value.startswith(REFERENCE_PREFIX):
        path = (base_dir / value[1:]).resolve()
        try:
            return base64.b64encode(path.read_bytes()).decode("ascii")
        except OSError as error:
            raise RunnerError(f"{value}: {error}") from error
    return value


def describe(value):
    return "missing" if value is MISSING else json.dumps(value, sort_keys=True)


def check(actual, expected, context):
    """Returns (problem or None, unrecorded flag) for one expectation."""
    is_matcher = isinstance(expected, dict) and expected and set(expected) <= MATCHER_KEYS
    if not is_matcher:
        expected = substitute(expected, context["saved"], context["base_dir"])
        if actual != expected:
            return f"expected {describe(expected)}, got {describe(actual)}", False
        return None, False

    if "recorded" in expected:
        key = expected["recorded"]
        if context["record"]:
            if actual is MISSING:
                return "nothing to record: the value is missing", False
            context["expected"].put(key, context["platform_key"], actual)
            return None, False
        stored = context["expected"].get(key, context["platform_key"])
        if stored is MISSING:
            return None, True
        if actual != stored:
            return f"expected recorded {key} {describe(stored)}, got {describe(actual)}", False

    problems = []
    if "equals" in expected:
        want = substitute(expected["equals"], context["saved"], context["base_dir"])
        if actual != want:
            problems.append(f"expected {describe(want)}")
    if "notEquals" in expected:
        avoid = substitute(expected["notEquals"], context["saved"], context["base_dir"])
        if actual == avoid:
            problems.append(f"expected anything but {describe(avoid)}")
    for bound, test in (("min", lambda a, b: a >= b), ("max", lambda a, b: a <= b)):
        if bound in expected:
            if not isinstance(actual, (int, float)) or isinstance(actual, bool) or not test(actual, expected[bound]):
                problems.append(f"expected {bound} {expected[bound]}")
    if problems:
        return ", ".join(problems) + f", got {describe(actual)}", False
    return None, False


# --- Running ---------------------------------------------------------------


def prepare_reference(arguments, context, step_report):
    """Resolves a "@ref:<name>" reference argument; returns the target path or None."""
    reference = arguments.get("reference")
    if not isinstance(reference, str) or not reference.startswith(REFERENCE_PREFIX):
        return None
    path = reference_path(context["scenario_path"], context["scenario_name"], context["platform_key"],
                          reference[len(REFERENCE_PREFIX):])
    step_report["reference"] = display_path(path)
    if context["record"]:
        del arguments["reference"]
        arguments["fullResolutionPng"] = True
        return path
    if not path.exists():
        del arguments["reference"]
        step_report["status"] = "unrecorded"
        return None
    arguments["reference"] = base64.b64encode(path.read_bytes()).decode("ascii")
    return None


def run_step(client, step, index, context):
    call = step.get("call")
    if not isinstance(call, str):
        raise RunnerError(f"{context['scenario_path']}: step {index} has no call")
    report = {"index": index, "call": call, "status": "pass", "problems": []}
    arguments = substitute(step.get("args", {}), context["saved"], context["base_dir"])
    record_reference_to = prepare_reference(arguments, context, report)
    reference_skipped = report["status"] == "unrecorded" or record_reference_to is not None

    started = time.monotonic()
    is_error, result, images = client.call(call, arguments)
    report["durationMs"] = round((time.monotonic() - started) * 1000)

    if is_error and not step.get("expectError"):
        report["status"] = "error"
        report["problems"].append(f"tool error: {result.get('error', result)}")
        return report

    if record_reference_to is not None:
        if not images:
            report["status"] = "error"
            report["problems"].append("recording a reference needs an image from the tool")
            return report
        record_reference_to.parent.mkdir(parents=True, exist_ok=True)
        record_reference_to.write_bytes(base64.b64decode(images[0]["data"]))
        report["recordedReference"] = True

    unrecorded = report["status"] == "unrecorded"
    for dotted, expected in step.get("expect", {}).items():
        if reference_skipped and dotted.split(".")[0] == "reference":
            continue
        problem, missing_record = check(lookup(result, dotted), expected, context)
        unrecorded |= missing_record
        if problem:
            report["problems"].append(f"{dotted}: {problem}")
    for name, dotted in step.get("save", {}).items():
        value = lookup(result, dotted)
        if value is MISSING:
            report["problems"].append(f"save {name}: {dotted} is missing")
        else:
            context["saved"][name] = value

    if report["problems"]:
        report["status"] = "fail"
    elif unrecorded:
        report["status"] = "unrecorded"
    return report


def run_scenario(client, path, args, platform_key):
    scenario = load_scenario(path)
    context = {
        "saved": {},
        "base_dir": Path(path).parent,
        "scenario_path": Path(path),
        "scenario_name": scenario["name"],
        "platform_key": platform_key,
        "record": args.record,
        "expected": Expected(path, scenario["name"]),
    }
    report = {"name": scenario["name"], "path": display_path(path), "steps": []}
    started = time.monotonic()

    # Each scenario starts without documents, so earlier ones cannot leak in.
    client.call("document_close", {"all": True})
    status = "pass"
    for index, step in enumerate(scenario["steps"], 1):
        step_report = run_step(client, step, index, context)
        report["steps"].append(step_report)
        if step_report["status"] in ("fail", "error"):
            status = "fail"
            if step_report["status"] == "error" or args.fail_fast:
                break
        elif step_report["status"] == "unrecorded" and status == "pass":
            status = "fail" if args.strict else "unrecorded"
    client.call("document_close", {"all": True})

    if args.record:
        context["expected"].save()
    report["status"] = status
    report["durationMs"] = round((time.monotonic() - started) * 1000)
    return report


def display_path(path):
    path = Path(path).resolve()
    try:
        return path.relative_to(REPO).as_posix()
    except ValueError:
        return str(path)


def platform_key_for(session):
    """"<os>-<cpu|gpu>": references and recorded values are kept per key."""
    system = platform.system().lower() or "unknown"
    environment = session.get("environment", {})
    gpu = any(environment.get(name) not in (None, "", "0") for name in ("KRITA_GPU_PROJECTION", "KRITA_GPU_BRUSH"))
    return f"{system}-{'gpu' if gpu else 'cpu'}"


# --- Launching -------------------------------------------------------------


class Launched:
    """A Solstice started by the runner with a scratch profile and test root."""

    def __init__(self, executable, port, language, extra_environment=None):
        self.directory = Path(tempfile.mkdtemp(prefix="solstice-scenarios-"))
        (self.directory / "profile" / "config").mkdir(parents=True)
        (self.directory / "root").mkdir()
        if language != "system":
            # The language override Settings > Switch Application Language
            # writes; read from the profile's config folder before the UI
            # starts, keyed by the application name (krita or solstice).
            (self.directory / "profile" / "config" / "klanguageoverridesrc").write_text(
                f"[Language]\nkrita={language}\nsolstice={language}\n", encoding="utf-8")
        self.token = secrets.token_hex(16)
        environment = dict(os.environ)
        environment.update({
            "SOLSTICE_PROFILE_ROOT": str(self.directory / "profile"),
            "SOLSTICE_TEST_CONTROL_PORT": str(port),
            "SOLSTICE_TEST_CONTROL_TOKEN": self.token,
            "SOLSTICE_TEST_CONTROL_ROOT": str(self.directory / "root"),
        })
        # For this process only, e.g. KRITA_PAINT_TRACE and the KRITA_GPU_*
        # switches that paint_trace_series.py sets per run.
        environment.update(extra_environment or {})
        self.log = open(self.directory / "solstice.log", "wb")
        self.process = subprocess.Popen([executable, "--nosplash"], env=environment,
                                        stdout=self.log, stderr=subprocess.STDOUT)

    def wait_ready(self, client, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RunnerError(f"Solstice exited with {self.process.returncode}; see {self.log.name}")
            try:
                client.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                              "clientInfo": {"name": "run_scenarios", "version": "1"}})
                _, session, _ = client.call("session_info", {})
                if session.get("mainWindows", 0) >= 1:
                    return
            except RunnerError:
                pass
            time.sleep(1)
        raise RunnerError(f"Solstice did not answer within {timeout} s; see {self.log.name}")

    def quit(self, client):
        try:
            client.call("document_close", {"all": True})
            client.call("action_trigger", {"name": "file_quit"})
        except RunnerError:
            pass
        try:
            self.process.wait(30)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            self.process.wait(30)
        self.log.close()
        shutil.rmtree(self.directory, ignore_errors=True)


# --- Main ------------------------------------------------------------------


def parse_arguments(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("scenarios", nargs="*", help="Scenario files")
    parser.add_argument("--all", action="store_true", help="Run every */tests/scenarios/*.json in the repository")
    parser.add_argument("--list", action="store_true", help="List the scenarios and exit")
    parser.add_argument("--port", type=int, default=int(os.environ.get("SOLSTICE_TEST_CONTROL_PORT", "8765")))
    parser.add_argument("--token", default=os.environ.get("SOLSTICE_TEST_CONTROL_TOKEN", ""))
    parser.add_argument("--report", help="Write a JSON report to this file")
    parser.add_argument("--record", action="store_true",
                        help="Store recorded values and reference images for this platform instead of comparing them")
    parser.add_argument("--strict", action="store_true", help="Treat missing recorded values and references as failures")
    parser.add_argument("--fail-fast", action="store_true", help="Stop a scenario at its first failing step")
    parser.add_argument("--launch", metavar="EXECUTABLE", help="Start this Solstice and quit it at the end")
    parser.add_argument("--launch-timeout", type=int, default=180, help="Seconds to wait for a launched Solstice")
    parser.add_argument("--language", default="en_US",
                        help="UI language of a launched Solstice (default en_US, which scenarios expect; 'system' keeps "
                             "the system language)")
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_arguments(argv)
    paths = [Path(p) for p in args.scenarios] + (discover(REPO) if args.all else [])
    if not paths:
        print("No scenarios given; name files or pass --all.", file=sys.stderr)
        return 2
    if args.list:
        for path in paths:
            print(display_path(path))
        return 0

    launched = None
    try:
        for path in paths:
            load_scenario(path)
        if args.launch:
            launched = Launched(args.launch, args.port, args.language)
            args.token = launched.token
        if len(args.token) < 16:
            raise RunnerError("No token: pass --token or set SOLSTICE_TEST_CONTROL_TOKEN")
        client = Client(f"http://127.0.0.1:{args.port}/mcp", args.token)
        if launched:
            launched.wait_ready(client, args.launch_timeout)
        _, session, _ = client.call("session_info", {})
        platform_key = platform_key_for(session)

        report = {
            "format": REPORT_FORMAT,
            "version": 1,
            "startedAt": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
            "platformKey": platform_key,
            "record": args.record,
            "solstice": {key: session.get(key) for key in ("solsticeVersion", "kritaCompatibilityVersion",
                                                             "qtVersion", "platform", "uiLanguages")},
            "scenarios": [],
        }
        print(f"Solstice {session.get('solsticeVersion')} on {session.get('platform')}, key {platform_key}")
        languages = session.get("uiLanguages") or []
        if languages and not str(languages[0]).startswith("en"):
            print(f"warning: the UI language is {languages[0]}; scenarios compare English texts, so text "
                  "expectations will fail. Use --launch, or switch Solstice to English.", file=sys.stderr)
        for path in paths:
            scenario_report = run_scenario(client, path, args, platform_key)
            report["scenarios"].append(scenario_report)
            print_scenario(scenario_report)
    except RunnerError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    finally:
        if launched:
            launched.quit(Client(f"http://127.0.0.1:{args.port}/mcp", launched.token, timeout=60))

    statuses = [scenario["status"] for scenario in report["scenarios"]]
    report["finishedAt"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
    report["totals"] = {status: statuses.count(status) for status in ("pass", "unrecorded", "fail")}
    if args.report:
        Path(args.report).write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    totals = report["totals"]
    print(f"{totals['pass']} passed, {totals['unrecorded']} passed with unrecorded values, {totals['fail']} failed")
    return 1 if totals["fail"] else 0


def print_scenario(report):
    print(f"{report['status'].upper():10} {report['name']} ({report['path']}, {report['durationMs']} ms)")
    for step in report["steps"]:
        if step["status"] != "pass":
            print(f"    step {step['index']} {step['call']}: {step['status']}")
            for problem in step["problems"]:
                print(f"      - {problem}")


if __name__ == "__main__":
    sys.exit(main())
