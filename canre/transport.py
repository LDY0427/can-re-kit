"""Ways to reach a vehicle bus.

- CanTransport: any adapter python-can supports (CANable/slcan, gs_usb/candleLight,
  PCAN, Kvaser, Vector, SocketCAN, ...). Can listen to raw frames and send requests.
- Elm327Transport: a cheap ELM327 (USB or Bluetooth serial). Requests only; the
  adapter hides raw bus traffic and adds its own latency.

Both expose `request()`; CanTransport also exposes `listen()`.
"""
from __future__ import annotations

import re
import time
from dataclasses import dataclass, field
from typing import Iterator, Optional

from . import isotp
from .safety import STANDARD_RX_IDS, check_request, check_tx_id

NRC_NAMES = {
    0x10: "general reject", 0x11: "service not supported", 0x12: "sub-function/ID not supported",
    0x13: "bad length/format", 0x21: "busy, repeat", 0x22: "conditions not correct",
    0x31: "request out of range", 0x33: "security access denied", 0x78: "response pending",
    0x7E: "not supported in session", 0x7F: "service not supported in session",
}


@dataclass
class Frame:
    t: float
    can_id: int
    data: bytes
    extended: bool = False
    error: bool = False


@dataclass
class Response:
    request: bytes
    status: str  # ok | nrc | timeout | partial
    rx_id: Optional[int] = None
    payload: bytes = b""
    latency_ms: Optional[float] = None
    nrc: Optional[int] = None
    t: float = field(default_factory=time.monotonic)

    @property
    def value(self) -> bytes:
        """Data after the positive-response service byte and echoed parameter bytes."""
        if self.status != "ok":
            return b""
        return self.payload[echo_len(self.request):]

    def describe(self) -> str:
        if self.status == "ok":
            return f"{self.rx_id:03X} {self.latency_ms:.1f} ms  {self.payload.hex(' ').upper()}"
        if self.status == "nrc":
            return f"negative response {self.nrc:02X} ({NRC_NAMES.get(self.nrc, '?')})"
        return self.status


def echo_len(req: bytes) -> int:
    """How many leading payload bytes repeat the request (service + parameter)."""
    sid = req[0]
    if sid in (0x01, 0x09, 0x21, 0x1A):
        return 2
    if sid == 0x22:
        return 3
    if sid == 0x02:
        return 3  # 42 PID frame
    if sid == 0x19:
        return 2
    return 1  # 03 07 0A: service byte only


def _matches(req: bytes, payload: bytes) -> bool:
    if not payload:
        return False
    if payload[0] == 0x7F:
        return len(payload) >= 2 and payload[1] == req[0]
    if payload[0] != (req[0] + 0x40) & 0xFF:
        return False
    n = min(echo_len(req), len(req))
    return payload[1:n] == req[1:n]


class Transport:
    name = "transport"

    def request(self, req: bytes, timeout: float = 0.2) -> Response:  # pragma: no cover - interface
        raise NotImplementedError

    def listen(self, seconds: float) -> Iterator[Frame]:  # pragma: no cover - interface
        raise NotImplementedError(f"{self.name} cannot show raw bus traffic; use a CAN adapter")
        yield

    def close(self) -> None:
        pass


class CanTransport(Transport):
    name = "can"

    def __init__(self, bus, tx_id: int = 0x7DF, allowed_tx: frozenset[int] = frozenset(),
                 force: bool = False, guard_s: float = 0.3):
        check_tx_id(tx_id, allowed_tx)
        self.bus = bus
        self.tx_id = tx_id
        self.allowed_tx = allowed_tx
        self.force = force
        self.guard_s = guard_s
        self._guarded = False
        self.sink = None  # optional callable(Frame): gets every frame that is not our answer

    # -- raw traffic
    def listen(self, seconds: float) -> Iterator[Frame]:
        end = time.monotonic() + seconds
        while (left := end - time.monotonic()) > 0:
            msg = self.bus.recv(timeout=min(left, 0.1))
            if msg is None:
                continue
            yield Frame(time.monotonic(), msg.arbitration_id, bytes(msg.data),
                        msg.is_extended_id, bool(getattr(msg, "is_error_frame", False)))

    # -- before the first transmission, make sure we are not about to talk over a broken bus
    def _guard(self) -> None:
        if self._guarded:
            return
        errors = sum(1 for f in self.listen(self.guard_s) if f.error)
        if errors and not self.force:
            raise RuntimeError(
                f"{errors} bus error frames in {self.guard_s:.1f} s before sending. "
                "Wrong bitrate or wiring? Fix that first (or --force if you know why)."
            )
        self._guarded = True

    def _pass_on(self, msg) -> None:
        if self.sink is not None and not getattr(msg, "is_error_frame", False):
            self.sink(Frame(time.monotonic(), msg.arbitration_id, bytes(msg.data), msg.is_extended_id))

    def _send(self, can_id: int, data: bytes) -> None:
        import can  # local import: python-can is only needed for this transport

        self.bus.send(can.Message(arbitration_id=can_id, data=data, is_extended_id=False))

    def _rx_ids(self) -> frozenset[int]:
        if self.tx_id == 0x7DF:
            return STANDARD_RX_IDS
        return frozenset({self.tx_id + 8})

    def request(self, req: bytes, timeout: float = 0.2) -> Response:
        check_request(req)
        check_tx_id(self.tx_id, self.allowed_tx)
        self._guard()
        while (old := self.bus.recv(timeout=0)) is not None:  # stale frames are not answers
            self._pass_on(old)
        rx_ids = self._rx_ids()
        asm: dict[int, isotp.Reassembler] = {}
        t0 = time.monotonic()
        self._send(self.tx_id, isotp.single_frame(req))
        deadline = t0 + timeout
        while (left := deadline - time.monotonic()) > 0:
            msg = self.bus.recv(timeout=left)
            if msg is None:
                break
            if msg.arbitration_id not in rx_ids or msg.is_extended_id:
                self._pass_on(msg)
                continue
            r = asm.setdefault(msg.arbitration_id, isotp.Reassembler())
            payload = r.feed(bytes(msg.data))
            if r.needs_flow_control:
                r.needs_flow_control = False
                self._send(msg.arbitration_id - 8, isotp.FLOW_CONTROL)
                deadline = max(deadline, time.monotonic() + 1.0)
            if payload is None or not _matches(req, payload):
                continue
            lat = (time.monotonic() - t0) * 1000
            if payload[0] == 0x7F:
                code = payload[2] if len(payload) > 2 else 0
                if code == 0x78:  # response pending: keep waiting
                    deadline = time.monotonic() + 5.0
                    continue
                return Response(req, "nrc", msg.arbitration_id, payload, lat, code)
            return Response(req, "ok", msg.arbitration_id, payload, lat)
        partial = any(r.in_progress for r in asm.values())
        return Response(req, "partial" if partial else "timeout")

    def close(self) -> None:
        self.bus.shutdown()


class Elm327Transport(Transport):
    """ELM327 over a serial port. Only AT commands that change nothing persistent are sent."""

    name = "elm327"
    PROTOCOLS = {(500, 11): "6", (500, 29): "7", (250, 11): "8", (250, 29): "9"}
    _FRAME = re.compile(r"^([0-9A-F]{3}|[0-9A-F]{8})((?: ?[0-9A-F]{2})+)$")

    def __init__(self, port: str, baud: int = 38400, bitrate_kbps: int = 500, tx_id: int = 0x7DF,
                 allowed_tx: frozenset[int] = frozenset(), ser=None):
        check_tx_id(tx_id, allowed_tx)
        if ser is None:
            import serial

            ser = serial.Serial(port, baud, timeout=0.1)
        self.ser = ser
        self.tx_id = tx_id
        proto = self.PROTOCOLS.get((bitrate_kbps, 11))
        if proto is None:
            raise ValueError("ELM327 supports 250 or 500 kbps here")
        for cmd in ("ATZ", "ATE0", "ATL0", "ATS1", "ATH1", "ATCAF1", f"ATSP{proto}", f"ATSH{tx_id:03X}"):
            self._cmd(cmd, wait=2.0 if cmd == "ATZ" else 1.0)

    def _cmd(self, line: str, wait: float = 1.0) -> list[str]:
        self.ser.reset_input_buffer()
        self.ser.write((line + "\r").encode())
        buf, end = b"", time.monotonic() + wait
        while time.monotonic() < end:
            chunk = self.ser.read(256)
            buf += chunk
            if buf.rstrip().endswith(b">"):
                break
        text = buf.decode(errors="replace").replace(">", "")
        return [ln.strip() for ln in text.replace("\r", "\n").split("\n") if ln.strip()]

    def request(self, req: bytes, timeout: float = 0.2) -> Response:
        check_request(req)
        t0 = time.monotonic()
        lines = self._cmd(req.hex().upper(), wait=max(timeout, 0.3) + 0.5)
        lat = (time.monotonic() - t0) * 1000
        asm: dict[int, isotp.Reassembler] = {}
        for ln in lines:
            m = self._FRAME.match(ln.upper())
            if not m:
                continue
            can_id = int(m.group(1), 16)
            data = bytes.fromhex(m.group(2).replace(" ", ""))
            r = asm.setdefault(can_id, isotp.Reassembler())
            payload = r.feed(data)
            r.needs_flow_control = False  # the ELM327 sends flow control itself
            if payload and _matches(req, payload):
                if payload[0] == 0x7F:
                    code = payload[2] if len(payload) > 2 else 0
                    return Response(req, "nrc", can_id, payload, lat, code)
                return Response(req, "ok", can_id, payload, lat)
        return Response(req, "timeout")

    def close(self) -> None:
        self.ser.close()
