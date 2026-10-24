# SPDX-License-Identifier: GPL-3.0-or-later
"""Session fixture: launch a real solstice.exe (or attach to a running one)
and hand out a configured Bridge client.

Config via environment:
  SOLSTICE_EXE   path to solstice.exe to launch (default: trial build dir)
  SOLSTICE_E2E_ATTACH=1   drive an already-running instance (port 5678)
"""

import os
import subprocess
import sys
import threading
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent))
from solstice_bridge import Bridge, BridgeError  # noqa: E402

DEFAULT_EXE = (
    r"F:\solstice-test\app-49697aad\Solstice-0.5.0-windows-x64-49697aadce6c\bin\solstice.exe"
    if Path(r"F:\solstice-test\app-49697aad\Solstice-0.5.0-windows-x64-49697aadce6c\bin\solstice.exe").exists()
    else r"F:\solstice-test\app-a50aa6f7\Solstice-0.2.0-beta.4-windows-x64-a50aa6f7ca63\bin\solstice.exe"
    if Path(r"F:\solstice-test\app-a50aa6f7\Solstice-0.2.0-beta.4-windows-x64-a50aa6f7ca63\bin\solstice.exe").exists()
    else r"F:\solstice-test\app-307c7e72\Solstice-0.2.0-beta.4-windows-x64-307c7e72c64f\bin\solstice.exe"
    if Path(r"F:\solstice-test\app-307c7e72\Solstice-0.2.0-beta.4-windows-x64-307c7e72c64f\bin\solstice.exe").exists()
    else r"F:\solstice-test\app\Solstice-0.2.0-beta.4-windows-x64-43e54bc88775\bin\solstice.exe"
)
HEALTH_TIMEOUT = 90  # plugin only binds after the main window creates actions


def pytest_configure(config):
    config.addinivalue_line("markers", "slow: long-running stability test")
    config.addinivalue_line("markers", "soak: repeated-operation soak test")


STDERR_LOG = Path(__file__).parent / "_last_run_stderr.log"


def _drain(stream, sink, fp):
    for line in iter(stream.readline, b""):
        sink.append(line)
        try:
            fp.write(line)
            fp.flush()
        except OSError:
            pass


class ManagedApp:
    def __init__(self):
        self.proc = None
        self.stderr_lines = []
        self._stderr_fp = None

    def start(self):
        exe = os.environ.get("SOLSTICE_EXE", DEFAULT_EXE)
        if not Path(exe).exists():
            pytest.skip(f"solstice.exe not found: {exe}")
        self._stderr_fp = open(STDERR_LOG, "wb")
        self.proc = subprocess.Popen(
            [exe],
            stdout=self._stderr_fp,
            stderr=subprocess.STDOUT,
        )
        # stdout+stderr merged into STDERR_LOG via the fd; also keep a copy
        # for the plugin-load assertions.
        threading.Thread(
            target=self._tail_log, daemon=True
        ).start()

    def _tail_log(self):
        try:
            with open(STDERR_LOG, "rb") as f:
                while True:
                    line = f.readline()
                    if line:
                        self.stderr_lines.append(line)
                    elif self.proc and self.proc.poll() is not None:
                        break
                    else:
                        time.sleep(0.5)
        except OSError:
            pass

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        if self._stderr_fp:
            self._stderr_fp.close()
            self._stderr_fp = None


@pytest.fixture(scope="session")
def app():
    attach = os.environ.get("SOLSTICE_E2E_ATTACH") == "1"
    bridge = Bridge()
    managed = ManagedApp()

    if attach:
        status, payload = bridge.health()
        if status != 200:
            pytest.skip("attach requested but no kritamcp bridge answers on :5678")
    else:
        try:
            status, _ = bridge.health()
            if status == 200:
                pytest.exit("a Solstice instance already serves :5678 — close it or set SOLSTICE_E2E_ATTACH=1")
        except BridgeError:
            pass  # nothing listening — expected before launch
        managed.start()
        deadline = time.time() + HEALTH_TIMEOUT
        while time.time() < deadline:
            if managed.proc.poll() is not None:
                pytest.exit(f"solstice.exe exited early (code {managed.proc.returncode})")
            try:
                status, _ = bridge.health()
                if status == 200:
                    break
            except BridgeError:
                pass
            time.sleep(2)
        else:
            managed.stop()
            pytest.exit(f"bridge did not answer /health within {HEALTH_TIMEOUT}s")

    yield bridge, managed

    if not attach:
        managed.stop()
