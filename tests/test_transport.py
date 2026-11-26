import itertools
from pathlib import Path

import pytest

can = pytest.importorskip("can")

from canre.__main__ import main
from canre.profile import ProfileError, load, to_dbc
from canre.safety import Refused
from canre.sim import SimEcu
from canre.transport import CanTransport, Elm327Transport

ROOT = Path(__file__).resolve().parents[1]
_n = itertools.count()


@pytest.fixture
def link():
    ch = f"test-{next(_n)}"
    ecu = SimEcu(ch).start()
    bus = can.Bus(interface="virtual", channel=ch, receive_own_messages=False)
    t = CanTransport(bus)
    yield t
    t.close()
    ecu.stop()


def test_single_frame_answer(link):
    r = link.request(b"\x21\x21")
    assert r.status == "ok" and r.rx_id == 0x7E8 and len(r.value) == 2


def test_multi_frame_answer_with_flow_control(link):
    r = link.request(b"\x03", timeout=0.5)
    assert r.status == "ok" and r.payload[0] == 0x43 and len(r.payload) == 14


def test_negative_response(link):
    r = link.request(b"\x21\x99")
    assert r.status == "nrc" and r.nrc == 0x12


def test_refused_before_anything_is_sent(link):
    with pytest.raises(Refused):
        link.request(b"\x14\xFF\xFF\xFF")


def test_guard_refuses_on_bus_errors():
    class ErrBus:
        def recv(self, timeout=None):
            return can.Message(is_error_frame=True)

        def send(self, msg):
            raise AssertionError("must not send")

        def shutdown(self):
            pass

    t = CanTransport(ErrBus(), guard_s=0.02)
    with pytest.raises(RuntimeError):
        t.request(b"\x01\x00")


class FakeElm:
    """Answers like an ELM327 with headers on (ATH1)."""

    def __init__(self):
        self.out = b""

    def reset_input_buffer(self):
        pass

    def write(self, data):
        cmd = data.decode().strip()
        if cmd.startswith("AT"):
            self.out = b"OK\r\r>"
        elif cmd == "2121":
            self.out = b"7E8 04 61 21 1A F8 \r\r>"
        elif cmd == "03":
            self.out = (b"7E8 10 0E 43 06 06 50 15 06 \r7E8 21 15 02 01 18 04 80 \r"
                        b"7E8 22 00 30 00 00 00 00 00 \r\r>")
        else:
            self.out = b"NO DATA\r\r>"

    def read(self, n):
        chunk, self.out = self.out[:n], self.out[n:]
        return chunk

    def close(self):
        pass


def test_elm327_parsing():
    t = Elm327Transport("x", ser=FakeElm())
    r = t.request(b"\x21\x21")
    assert r.status == "ok" and r.value == b"\x1A\xF8"
    r = t.request(b"\x03")
    assert r.status == "ok" and len(r.payload) == 14
    assert t.request(b"\x21\x30").status == "timeout"
    with pytest.raises(Refused):
        t.request(b"\x04")


# ---------------------------------------------------------------- profiles

@pytest.mark.parametrize("path", sorted((ROOT / "profiles").glob("*.yaml")))
def test_bundled_profiles_are_valid(path):
    p = load(path)
    assert p.meta
    to_dbc(p)


def test_profile_needs_evidence(tmp_path):
    f = tmp_path / "bad.yaml"
    f.write_text("vehicle: {make: X}\ndiag_signals:\n  - {name: rpm, request: '21 21', confidence: measured}\n")
    with pytest.raises(ProfileError):
        load(f)


def test_profile_decode():
    p = load(ROOT / "profiles" / "ktm-390-duke-2016.yaml")
    assert p.by_name("rpm").decode(b"\x1A\xF8") == pytest.approx(1726)


def test_cli_end_to_end(tmp_path, capsys):
    log = tmp_path / "p.csv"
    assert main(["poll", "21 3B", "01 11", "-n", "60", "--interval", "20", "-q", "--log", str(log)]) == 0
    assert main(["analyze", str(log), "--reference", "01 11", "--top", "3"]) == 0
    out = capsys.readouterr().out
    assert "21 3B" in out.split("chance threshold")[1].splitlines()[1]
    assert main(["req", "27", "01"]) == 3


def test_broadcast_bytes_found_against_polled_value(tmp_path, capsys):
    log = tmp_path / "b.csv"
    assert main(["poll", "21 21", "-n", "80", "--interval", "25", "-q", "--sim-broadcast",
                 "--record-all", "--log", str(log)]) == 0
    capsys.readouterr()
    assert main(["analyze", str(log), "--reference", "21 21:u16be*0.25", "--top", "1"]) == 0
    best = capsys.readouterr().out.split("(Bonferroni)")[1].splitlines()[1]
    assert best.startswith("ID 120") and "byte 0 u16be" in best
