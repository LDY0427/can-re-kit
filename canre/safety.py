"""Read-only transmit policy.

Every frame this tool puts on a vehicle bus passes through `check_request`.
Only *read* diagnostic services are allowed. There is deliberately no flag to
turn this off: writing, clearing codes, security access, routines, resets and
flashing are out of scope for this project.
"""
from __future__ import annotations

# Services that only read data.
READ_SERVICES: dict[int, str] = {
    0x01: "OBD current data",
    0x02: "OBD freeze frame",
    0x03: "OBD stored DTCs",
    0x07: "OBD pending DTCs",
    0x09: "OBD vehicle info",
    0x0A: "OBD permanent DTCs",
    0x19: "UDS ReadDTCInformation",
    0x1A: "KWP ReadEcuIdentification",
    0x21: "KWP ReadDataByLocalIdentifier",
    0x22: "UDS ReadDataByIdentifier",
}

# Named here only so the refusal message says what was blocked.
BLOCKED_SERVICES: dict[int, str] = {
    0x04: "OBD clear DTCs",
    0x08: "OBD actuator test",
    0x10: "session control",
    0x11: "ECU reset",
    0x14: "clear DTCs",
    0x23: "read memory by address",
    0x27: "security access",
    0x28: "communication control",
    0x2E: "write data",
    0x2F: "I/O control",
    0x30: "I/O control (KWP)",
    0x31: "routine control",
    0x34: "request download",
    0x35: "request upload",
    0x36: "transfer data",
    0x37: "transfer exit",
    0x3B: "write data (KWP)",
    0x3D: "write memory",
    0x3E: "tester present",
    0x85: "control DTC setting",
}

# OBD-II addressing (ISO 15765-4): functional 0x7DF, physical 0x7E0..0x7E7.
STANDARD_TX_IDS = frozenset({0x7DF, *range(0x7E0, 0x7E8)})
STANDARD_RX_IDS = frozenset(range(0x7E8, 0x7F0))


class Refused(ValueError):
    """A request the read-only policy does not allow."""


def check_request(req: bytes) -> None:
    """Raise Refused unless `req` is a single-frame read request."""
    if not req:
        raise Refused("empty request")
    if len(req) > 7:
        raise Refused(f"only single-frame requests (<= 7 bytes) are sent, got {len(req)}")
    sid = req[0]
    if sid not in READ_SERVICES:
        name = BLOCKED_SERVICES.get(sid, "not a read service")
        allowed = " ".join(f"{s:02X}" for s in sorted(READ_SERVICES))
        raise Refused(f"blocked service {sid:02X} ({name}); allowed read services: {allowed}")


def check_tx_id(tx_id: int, extra_allowed: frozenset[int] = frozenset()) -> None:
    """Only the OBD request IDs, plus IDs the user named explicitly."""
    if tx_id in STANDARD_TX_IDS or tx_id in extra_allowed:
        return
    raise Refused(
        f"request ID {tx_id:03X} is not an OBD request ID (7DF, 7E0-7E7). "
        "If your vehicle's diagnostic address is different and you have confirmed it, "
        "pass --allow-tx-id with that ID."
    )


def parse_hex_bytes(text: str | list[str]) -> bytes:
    """'21 21' / '2121' / ['21', '21'] -> b'\\x21\\x21'. Each token 1-2 hex digits or an even run."""
    tokens = text.split() if isinstance(text, str) else [t for part in text for t in part.split()]
    out = bytearray()
    for tok in tokens:
        tok = tok.removeprefix("0x").removeprefix("0X")
        if len(tok) <= 2:
            out.append(int(tok, 16))
        elif len(tok) % 2 == 0:
            out.extend(bytes.fromhex(tok))
        else:
            raise ValueError(f"bad hex token: {tok!r}")
    return bytes(out)
