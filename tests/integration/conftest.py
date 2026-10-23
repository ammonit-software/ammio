"""Shared fixtures for the ammio integration tests.

The tests start the real ammio binary against simulated devices (pymodbus, asyncua) and talk to
it over its JSON API. Point AMMIO_BIN at the binary, e.g.:

    AMMIO_BIN=build/ammio pytest tests/integration
"""

import json
import os
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

import pynng
import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_until(predicate, timeout=5.0, interval=0.02, message="condition"):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = predicate()
        if last:
            return last
        time.sleep(interval)
    raise AssertionError(f"timed out waiting for {message} (last result: {last!r})")


class AmmioClient:
    def __init__(self, endpoint: str):
        self.sock = pynng.Req0(dial=endpoint, recv_timeout=3000, send_timeout=3000)

    def request(self, req: dict) -> dict:
        self.sock.send(json.dumps(req).encode())
        return json.loads(self.sock.recv().decode())

    def raw(self, data: bytes) -> dict:
        self.sock.send(data)
        return json.loads(self.sock.recv().decode())

    def read(self, var_id: str) -> dict:
        resp = self.request({"cmd": "read", "var_id": var_id})
        assert resp["ok"], resp
        return resp

    def value(self, var_id: str):
        return self.read(var_id)["value"]

    def write(self, var_id: str, value) -> dict:
        return self.request({"cmd": "write", "var_id": var_id, "value": value})

    def close(self):
        self.sock.close()


class AmmioProcess:
    def __init__(self, binary: Path, config: dict, tmp_path: Path, events: bool = False):
        self.config_path = tmp_path / "ammio.json"
        self.config_path.write_text(json.dumps(config, indent=2))
        self.endpoint = f"tcp://127.0.0.1:{free_port()}"
        self.events_endpoint = f"tcp://127.0.0.1:{free_port()}" if events else None
        self.log_path = tmp_path / "ammio.log"
        args = [str(binary), "--config", str(self.config_path), "--endpoint", self.endpoint,
                "--log-level", "debug"]
        if self.events_endpoint:
            args += ["--events", self.events_endpoint]
        self.log = open(self.log_path, "w")
        # On Windows a graceful stop needs Ctrl+Break, which requires a separate process group
        flags = subprocess.CREATE_NEW_PROCESS_GROUP if sys.platform == "win32" else 0
        self.proc = subprocess.Popen(args, stdout=self.log, stderr=subprocess.STDOUT, creationflags=flags)
        self.client = None
        try:
            self.client = AmmioClient(self.endpoint)
            wait_until(self._ready, timeout=10, message="ammio API")
        except Exception:
            self.stop()
            raise

    def _ready(self):
        if self.proc.poll() is not None:
            raise AssertionError(f"ammio exited with {self.proc.returncode}:\n{self.output()}")
        try:
            return self.client.request({"cmd": "info"}).get("ok")
        except pynng.exceptions.Timeout:
            # A REQ socket must be recreated after a timed-out request
            self.client.close()
            self.client = AmmioClient(self.endpoint)
            return False

    def output(self) -> str:
        self.log.flush()
        return self.log_path.read_text()

    def stop(self) -> int:
        if self.client:
            self.client.close()
        if self.proc.poll() is None:
            if sys.platform == "win32":
                os.kill(self.proc.pid, signal.CTRL_BREAK_EVENT)
            else:
                self.proc.terminate()
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.log.close()
        return self.proc.returncode


@pytest.fixture(scope="session")
def ammio_bin() -> Path:
    path = Path(os.environ.get("AMMIO_BIN", REPO_ROOT / "build" / "ammio"))
    if not path.exists():
        pytest.skip(f"ammio binary not found at {path} (set AMMIO_BIN)")
    return path


@pytest.fixture
def start_ammio(ammio_bin, tmp_path):
    procs = []

    def start(config: dict, events: bool = False) -> AmmioProcess:
        p = AmmioProcess(ammio_bin, config, tmp_path, events=events)
        procs.append(p)
        return p

    yield start
    for p in procs:
        rc = p.stop()
        assert rc == 0, f"ammio did not shut down cleanly (exit {rc}):\n{p.output()}"


@pytest.fixture
def run_check(ammio_bin, tmp_path):
    """Runs `ammio --check` on a config and returns (exit code, output)."""

    def run(config) -> tuple[int, str]:
        path = tmp_path / "check.json"
        path.write_text(config if isinstance(config, str) else json.dumps(config))
        r = subprocess.run([str(ammio_bin), "--check", "--config", str(path)],
                           capture_output=True, text=True, timeout=30)
        return r.returncode, r.stdout + r.stderr

    return run
