"""canre <-> the real firmware core, through its WebAssembly build (firmware/test/host_build.py sim)."""
import shutil
import subprocess
from pathlib import Path

import pytest

from canre.safety import Refused
from canre.transport import SctTransport

ROOT = Path(__file__).resolve().parents[1]
SIM = ROOT / "firmware" / "test" / "build" / "sim.wasm"
RUNNER = ROOT / "firmware" / "test" / "sim" / "run_sim.mjs"

pytestmark = pytest.mark.skipif(not (shutil.which("node") and SIM.exists()),
                                reason="needs node and firmware/test/build/sim.wasm (python firmware/test/host_build.py sim)")


class PipeSerial:
    def __init__(self):
        self.p = subprocess.Popen(["node", "--no-warnings", str(RUNNER)], cwd=RUNNER.parent.parent.parent,
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def write(self, b):
        self.p.stdin.write(b)
        self.p.stdin.flush()

    def readline(self):
        return self.p.stdout.readline()

    def close(self):
        self.p.stdin.close()
        self.p.wait(timeout=10)


@pytest.fixture
def fw():
    t = SctTransport("sim", ser=PipeSerial())
    yield t
    t.close()


def test_hello(fw):
    assert fw.hello.startswith("HELLO,stock-can-tool,")


def test_single_and_multi_frame(fw):
    r = fw.request(b"\x21\x21")
    assert r.status == "ok" and r.payload[:2] == b"\x61\x21" and r.latency_ms > 0
    r = fw.request(b"\x03")
    assert r.status == "ok" and r.payload[0] == 0x43 and len(r.payload) == 26


def test_nrc_and_no_answer(fw):
    assert fw.request(b"\x21\x99").status in ("nrc", "timeout")
    assert fw.request(b"\x01\x0C").status == "timeout"  # like the real ECU: no standard rpm


def test_policy_checked_before_sending(fw):
    with pytest.raises(Refused):
        fw.request(b"\x2E\x00\x00")
