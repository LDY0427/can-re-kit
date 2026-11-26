"""Minimal ISO 15765-2 (ISO-TP) for diagnostic request/response.

Requests are always single frames (the read-only policy limits them to 7 bytes).
Responses may be single or multi frame; for a first frame we answer with a
flow-control frame (continue, block size 0, STmin 0) on response ID - 8, which
is the OBD-II convention.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Optional

PAD = 0x00


def single_frame(payload: bytes) -> bytes:
    if not 1 <= len(payload) <= 7:
        raise ValueError("single frame payload must be 1..7 bytes")
    return bytes([len(payload)]) + payload + bytes([PAD] * (7 - len(payload)))


FLOW_CONTROL = bytes([0x30, 0x00, 0x00, 0, 0, 0, 0, 0])


@dataclass
class Reassembler:
    """Feed frames from one response ID; `feed` returns the payload when complete.

    `needs_flow_control` becomes True after a first frame: the caller must send
    FLOW_CONTROL and then keep feeding consecutive frames.
    """

    expected: int = 0
    buf: bytearray = field(default_factory=bytearray)
    next_sn: int = 1
    needs_flow_control: bool = False
    error: Optional[str] = None

    def reset(self) -> None:
        self.expected, self.buf, self.next_sn = 0, bytearray(), 1
        self.needs_flow_control, self.error = False, None

    @property
    def in_progress(self) -> bool:
        return self.expected > 0

    def feed(self, data: bytes) -> Optional[bytes]:
        if not data:
            return None
        kind = data[0] >> 4
        if kind == 0:  # single frame
            n = data[0] & 0x0F
            if n == 0 or n > len(data) - 1:
                self.error = "bad single-frame length"
                return None
            self.reset()
            return bytes(data[1:1 + n])
        if kind == 1:  # first frame
            n = ((data[0] & 0x0F) << 8) | data[1]
            self.reset()
            self.expected = n
            self.buf.extend(data[2:8])
            self.needs_flow_control = True
            return None
        if kind == 2:  # consecutive frame
            if not self.in_progress:
                return None
            sn = data[0] & 0x0F
            if sn != self.next_sn:
                msg = f"sequence {sn} != expected {self.next_sn}"
                self.reset()
                self.error = msg
                return None
            self.next_sn = (self.next_sn + 1) & 0x0F
            self.buf.extend(data[1:8])
            if len(self.buf) >= self.expected:
                out = bytes(self.buf[: self.expected])
                self.reset()
                return out
            return None
        return None  # flow control or unknown: not part of a response


def segment(payload: bytes) -> list[bytes]:
    """Split a payload into ISO-TP frames (used by the simulated ECU)."""
    if len(payload) <= 7:
        return [single_frame(payload)]
    n = len(payload)
    frames = [bytes([0x10 | (n >> 8), n & 0xFF]) + payload[:6]]
    rest, sn = payload[6:], 1
    while rest:
        chunk, rest = rest[:7], rest[7:]
        frames.append(bytes([0x20 | sn]) + chunk + bytes([PAD] * (7 - len(chunk))))
        sn = (sn + 1) & 0x0F
    return frames
