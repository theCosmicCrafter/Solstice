#!/usr/bin/env python3
"""Verify the local Solstice toolchain against windows-dev.lock.json.

Report-only: prints ok / missing / mismatch per tool and never installs
or modifies anything. Exit 1 when any *required* tool fails; optional
(phase-gated) tools report warnings without failing.

Probe kinds supported by the lock file:
  command    run probe.run via the shell, regex the output for a version
  env-regex  regex the named environment variable's value for a version
  env-dir    check the named env var points at an existing directory

Version rules: min (semver-ish >=), exact-string, exact-major-minor.

Run inside an `env.bat` shell for meaningful results — LLVM-MinGW and
the Vulkan SDK live behind the dev-environment PATH, not globally.

Usage: python check-local-toolchains.py [path-to-lock.json]
"""

import json
import os
import re
import subprocess
import sys

LOCK = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                    "windows-dev.lock.json")


def version_tuple(text):
    return tuple(int(p) for p in re.findall(r"\d+", text)[:4])


def version_ok(found, rule):
    if found is None:
        return False
    if "exact-string" in rule:
        return found == rule["exact-string"] or rule["exact-string"] in found
    if "exact-major-minor" in rule:
        want = rule["exact-major-minor"].split(".")[:2]
        got = found.split(".")
        return got[:2] == want
    if "min" in rule:
        return version_tuple(found) >= version_tuple(rule["min"])
    return True


def probe_command(spec):
    try:
        out = subprocess.run(
            spec["run"], shell=True, capture_output=True, text=True,
            timeout=30)
        text = out.stdout + out.stderr
    except (OSError, subprocess.TimeoutExpired):
        return None, "command failed"
    m = re.search(spec["regex"], text)
    if not m:
        return None, "not found / no version in output"
    return m.group(1), None


def probe_env_regex(spec):
    value = os.environ.get(spec["env"], "")
    if not value:
        return None, f"{spec['env']} not set"
    m = re.search(spec["regex"], value)
    return (m.group(1) if m else value), None


def probe_env_dir(spec):
    value = os.environ.get(spec["env"], "")
    if not value:
        return None, f"{spec['env']} not set"
    if not os.path.isdir(value):
        return None, f"{spec['env']} -> {value} (not a directory)"
    return value, None


PROBES = {
    "command": probe_command,
    "env-regex": probe_env_regex,
    "env-dir": probe_env_dir,
}


def check(tool):
    spec = tool["probe"]
    found, err = PROBES[spec["kind"]](spec)
    if err and spec.get("fallback-env"):
        # command probe failed; fall back to "env var points at something"
        if os.environ.get(spec["fallback-env"]):
            return "present (version unverified)", "ok"
        return err, "missing"
    if err:
        return err, "missing"
    if not version_ok(found, tool["version"]):
        return f"found {found}", "mismatch"
    return f"found {found}", "ok"


def main():
    lock_path = sys.argv[1] if len(sys.argv) > 1 else LOCK
    with open(lock_path, encoding="utf-8") as fh:
        lock = json.load(fh)
    print(f"toolchain check against {os.path.basename(lock_path)}"
          f" (updated {lock.get('updated')})")
    failures = warnings = 0
    for tool in lock["tools"]:
        detail, status = check(tool)
        tag = {"ok": "OK ", "missing": "MISSING", "mismatch": "MISMATCH"}[status]
        level = "" if tool["required"] else f"  (optional — {tool['gates']})"
        if status == "ok":
            print(f"  [{tag}] {tool['name']}: {detail}{level}")
        elif tool["required"]:
            failures += 1
            print(f"  [{tag}] {tool['name']}: {detail} — REQUIRED")
        else:
            warnings += 1
            print(f"  [{tag}] {tool['name']}: {detail}{level}")
    print(f"{failures} required failure(s), {warnings} optional warning(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
