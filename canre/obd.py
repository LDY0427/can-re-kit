"""SAE J1979 (OBD-II) standard items and DTC decoding.

Formulas are the public standard ones. Use them as *reference signals*: once a
standard value is readable, it is the ruler you hold manufacturer-specific
values against.
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Callable, Optional


def _u16(d: bytes) -> int:
    return d[0] << 8 | d[1]


@dataclass(frozen=True)
class Pid:
    pid: int
    name: str
    unit: str
    nbytes: int
    decode: Callable[[bytes], float]


PIDS: dict[int, Pid] = {p.pid: p for p in [
    Pid(0x04, "engine_load", "%", 1, lambda d: d[0] * 100 / 255),
    Pid(0x05, "coolant_temp", "C", 1, lambda d: d[0] - 40),
    Pid(0x06, "stft_b1", "%", 1, lambda d: d[0] * 100 / 128 - 100),
    Pid(0x07, "ltft_b1", "%", 1, lambda d: d[0] * 100 / 128 - 100),
    Pid(0x0A, "fuel_pressure", "kPa", 1, lambda d: d[0] * 3),
    Pid(0x0B, "map", "kPa", 1, lambda d: d[0]),
    Pid(0x0C, "rpm", "rpm", 2, lambda d: _u16(d) / 4),
    Pid(0x0D, "speed", "km/h", 1, lambda d: d[0]),
    Pid(0x0E, "ignition_advance", "deg", 1, lambda d: d[0] / 2 - 64),
    Pid(0x0F, "intake_temp", "C", 1, lambda d: d[0] - 40),
    Pid(0x10, "maf", "g/s", 2, lambda d: _u16(d) / 100),
    Pid(0x11, "throttle", "%", 1, lambda d: d[0] * 100 / 255),
    Pid(0x14, "o2_b1s1_v", "V", 2, lambda d: d[0] / 200),
    Pid(0x1F, "run_time", "s", 2, _u16),
    Pid(0x2F, "fuel_level", "%", 1, lambda d: d[0] * 100 / 255),
    Pid(0x33, "baro", "kPa", 1, lambda d: d[0]),
    Pid(0x42, "ecu_voltage", "V", 2, lambda d: _u16(d) / 1000),
    Pid(0x45, "relative_throttle", "%", 1, lambda d: d[0] * 100 / 255),
    Pid(0x46, "ambient_temp", "C", 1, lambda d: d[0] - 40),
    Pid(0x5C, "oil_temp", "C", 1, lambda d: d[0] - 40),
]}

SUPPORT_PIDS = (0x00, 0x20, 0x40, 0x60, 0x80, 0xA0, 0xC0)


def supported_from_bitmap(base: int, bitmap: bytes) -> list[int]:
    """`01 00` answers 4 bytes: bit 31 = PID base+1 ... bit 0 = PID base+32."""
    if len(bitmap) < 4:
        return []
    word = int.from_bytes(bitmap[:4], "big")
    return [base + i + 1 for i in range(32) if word & (1 << (31 - i))]


def decode_pid(pid: int, value: bytes) -> Optional[float]:
    p = PIDS.get(pid)
    if p is None or len(value) < p.nbytes:
        return None
    return p.decode(value)


_DTC_LETTER = "PCBU"


def dtc_code(hi: int, lo: int) -> str:
    return f"{_DTC_LETTER[hi >> 6]}{(hi >> 4) & 0x3}{hi & 0xF:X}{lo:02X}"


def decode_dtcs(payload: bytes) -> tuple[list[str], bool]:
    """Payload of a 43/47/4A response. Returns (codes, count_mismatch).

    On CAN the service byte is followed by a count byte. Some ECUs count the
    0000 padding pairs too, so: if count*2 fits use count pairs, otherwise read
    every remaining pair and flag the mismatch. 0000 is padding and skipped.
    """
    if len(payload) < 2:
        return [], False
    count, body = payload[1], payload[2:]
    pairs = len(body) // 2
    mismatch = count > pairs
    n = pairs if mismatch else count
    codes = []
    for i in range(n):
        hi, lo = body[2 * i], body[2 * i + 1]
        if hi == 0 and lo == 0:
            continue
        codes.append(dtc_code(hi, lo))
    return codes, mismatch
