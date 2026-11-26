"""A simulated single-cylinder motorcycle ECU on a python-can virtual bus.

It is fictional but behaves like the real ECU in the case study, so every
command and the whole workflow can be practised without a vehicle:

- 500 kbps, 11-bit, requests on 7DF/7E0, answers on 7E8
- standard RPM (01 0C) is NOT supported; RPM is only in manufacturer data (21 21)
- manufacturer local IDs 21 20..21 66 answer, others get NRC 12
- stored DTCs (03) need a multi-frame answer
- optional periodic broadcast frame (off by default; the real ECU sent none)
"""
from __future__ import annotations

import math
import threading
import time

from . import isotp
from .obd import SUPPORT_PIDS

SUPPORTED_01 = (0x05, 0x0B, 0x0D, 0x0E, 0x0F, 0x11)
STORED_DTCS = [(0x06, 0x50), (0x15, 0x06), (0x15, 0x02), (0x01, 0x18), (0x04, 0x80), (0x00, 0x30)]
LOCAL_IDS = range(0x20, 0x67)


class EngineModel:
    """Deterministic engine state as a function of time since start."""

    def __init__(self, period_s: float = 20.0):
        self.t0 = time.monotonic()
        self.period = period_s
        self._revs = 0.0
        self._last = 0.0

    def state(self) -> dict[str, float]:
        t = time.monotonic() - self.t0
        # throttle and rpm move on different rhythms so they can be told apart
        phase = 0.5 - 0.5 * math.cos(2 * math.pi * t / self.period)
        tphase = 0.5 - 0.5 * math.cos(2 * math.pi * t / (self.period * 0.37))
        rpm = 1900 + 6000 * phase
        throttle = 100 * tphase
        self._revs += rpm / 60 * max(0.0, t - self._last)
        self._last = t
        return {
            "rpm": rpm,
            "throttle_pct": throttle,
            "map_kpa": 30 + 45 * tphase + 25 * phase,
            "coolant_c": min(30 + 0.5 * t, 92),
            "intake_c": 24.0,
            "advance_deg": 10 + 20 * phase - 6 * tphase,
            "speed": 0.0,
            "revs": self._revs,
        }


class SimEcu:
    def __init__(self, channel: str = "canre-sim", broadcast: bool = False, engine: EngineModel | None = None):
        import can

        self.bus = can.Bus(interface="virtual", channel=channel, receive_own_messages=False)
        self.engine = engine or EngineModel()
        self.broadcast = broadcast
        self._stop = threading.Event()
        self._pending: list[bytes] = []
        self._fc = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)

    # -- lifecycle
    def start(self) -> "SimEcu":
        self._thread.start()
        return self

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=1)
        self.bus.shutdown()

    # -- answers
    def answer(self, req: bytes) -> bytes:
        s = self.engine.state()
        sid = req[0]
        if sid == 0x01 and len(req) >= 2:
            pid = req[1]
            if pid in SUPPORT_PIDS:
                word = 0
                for p in SUPPORTED_01:
                    if pid < p <= pid + 32:
                        word |= 1 << (31 - (p - pid - 1))
                return bytes([0x41, pid]) + word.to_bytes(4, "big")
            enc = {
                0x05: lambda: [int(s["coolant_c"] + 40)],
                0x0B: lambda: [int(s["map_kpa"])],
                0x0D: lambda: [0],
                0x0E: lambda: [int((s["advance_deg"] + 64) * 2)],
                0x0F: lambda: [int(s["intake_c"] + 40)],
                0x11: lambda: [int(s["throttle_pct"] * 255 / 100)],
            }.get(pid)
            return bytes([0x41, pid, *enc()]) if enc else bytes([0x7F, 0x01, 0x12])
        if sid == 0x03:
            body = b"".join(bytes(p) for p in STORED_DTCS)
            return bytes([0x43, len(STORED_DTCS)]) + body
        if sid == 0x07:
            return bytes([0x47, 1, 0x15, 0x05])
        if sid == 0x21 and len(req) >= 2:
            lid = req[1]
            if lid not in LOCAL_IDS:
                return bytes([0x7F, 0x21, 0x12])
            u16 = lambda v: int(max(0, min(65535, v))).to_bytes(2, "big")
            val = {
                0x21: lambda: u16(s["rpm"] * 4),
                0x22: lambda: bytes([int((s["coolant_c"] + 48) / 0.75)]),
                0x2A: lambda: bytes([int(7 + s["throttle_pct"] * 2.46)]),
                0x3B: lambda: u16(680 + 30 * s["throttle_pct"]),
                0x42: lambda: bytes([1 if s["rpm"] > 300 else 0]),
                0x4A: lambda: u16(s["map_kpa"] * 256),
                0x64: lambda: int(s["revs"]).to_bytes(4, "big"),
            }.get(lid, lambda: bytes([lid ^ 0x5A, 0x00]))
            return bytes([0x61, lid]) + val()
        return bytes([0x7F, sid, 0x11])

    def _send(self, can_id: int, data: bytes) -> None:
        import can

        self.bus.send(can.Message(arbitration_id=can_id, data=data, is_extended_id=False))

    def _run(self) -> None:
        next_bc = time.monotonic()
        counter = 0
        while not self._stop.is_set():
            msg = self.bus.recv(timeout=0.002)
            if msg is not None and not msg.is_extended_id:
                if msg.arbitration_id in (0x7DF, 0x7E0):
                    n = msg.data[0] & 0x0F
                    if msg.data[0] >> 4 == 0 and 1 <= n <= 7:
                        frames = isotp.segment(self.answer(bytes(msg.data[1:1 + n])))
                        time.sleep(0.001)
                        self._send(0x7E8, frames[0])
                        self._pending = frames[1:]
                    elif msg.data[0] == 0x30 and self._pending and msg.arbitration_id == 0x7E0:
                        for f in self._pending:
                            self._send(0x7E8, f)
                        self._pending = []
            if self.broadcast and time.monotonic() >= next_bc:
                s = self.engine.state()
                rpm = int(s["rpm"]).to_bytes(2, "big")
                self._send(0x120, rpm + bytes([int(s["throttle_pct"] * 2.55), 0, 0, 0, 0, counter & 0xFF]))
                counter += 1
                next_bc += 0.01
