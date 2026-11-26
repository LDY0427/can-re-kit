"""Vehicle profiles: what is known about one vehicle's bus, and how sure we are.

A profile is a YAML file in `profiles/`. Every signal carries a `confidence`:
  measured  - checked on the vehicle against an independent reference
  inferred  - pattern in our logs, not yet confirmed by a deliberate test
  reported  - someone else's claim (forum, article), not checked by us
  standard  - public standard formula (SAE J1979 etc.)
Never mix them up: a profile is only useful if a reader can tell which is which.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any, Optional

import yaml

CONFIDENCE = ("measured", "inferred", "reported", "standard")
LAYOUT_ENDIAN = {"big", "little"}


class ProfileError(ValueError):
    pass


@dataclass
class DiagSignal:
    name: str
    request: bytes
    byte: int
    length: int
    endian: str
    scale: Optional[float]
    offset: float
    unit: str
    confidence: str
    evidence: str

    def decode(self, value: bytes) -> Optional[float]:
        if len(value) < self.byte + self.length:
            return None
        raw = int.from_bytes(value[self.byte:self.byte + self.length], self.endian)
        return raw if self.scale is None else raw * self.scale + self.offset


@dataclass
class BroadcastSignal:
    name: str
    can_id: int
    start_byte: int
    length: int
    endian: str
    scale: float
    offset: float
    unit: str
    confidence: str
    evidence: str


@dataclass
class Profile:
    path: Path
    meta: dict[str, Any]
    bus: dict[str, Any]
    diag: list[DiagSignal]
    broadcast: list[BroadcastSignal]

    def by_name(self, name: str) -> DiagSignal:
        for s in self.diag:
            if s.name == name:
                return s
        raise KeyError(name)


def _req(text: str) -> bytes:
    return bytes(int(t, 16) for t in str(text).split())


def _need(d: dict, key: str, where: str):
    if key not in d:
        raise ProfileError(f"{where}: missing '{key}'")
    return d[key]


def _conf(d: dict, where: str) -> str:
    c = _need(d, "confidence", where)
    if c not in CONFIDENCE:
        raise ProfileError(f"{where}: confidence must be one of {CONFIDENCE}, got {c!r}")
    if c in ("measured", "inferred", "reported") and not str(d.get("evidence", "")).strip():
        raise ProfileError(f"{where}: '{c}' needs an evidence line (what was compared, how many samples)")
    return c


def load(path: str | Path) -> Profile:
    path = Path(path)
    doc = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
    meta = _need(doc, "vehicle", path.name)
    bus = doc.get("bus", {})
    diag, bc = [], []
    seen: set[str] = set()
    for i, s in enumerate(doc.get("diag_signals", []) or []):
        where = f"{path.name} diag_signals[{i}] {s.get('name', '?')}"
        name = _need(s, "name", where)
        if name in seen:
            raise ProfileError(f"{where}: duplicate name")
        seen.add(name)
        endian = s.get("endian", "big")
        if endian not in LAYOUT_ENDIAN:
            raise ProfileError(f"{where}: endian must be big or little")
        diag.append(DiagSignal(name, _req(_need(s, "request", where)), int(s.get("byte", 0)),
                               int(s.get("length", 1)), endian, s.get("scale"), float(s.get("offset", 0)),
                               s.get("unit", ""), _conf(s, where), str(s.get("evidence", ""))))
    for i, s in enumerate(doc.get("broadcast_signals", []) or []):
        where = f"{path.name} broadcast_signals[{i}] {s.get('name', '?')}"
        bc.append(BroadcastSignal(_need(s, "name", where), int(str(_need(s, "id", where)), 16),
                                  int(s.get("start_byte", 0)), int(s.get("length", 1)), s.get("endian", "big"),
                                  float(s.get("scale", 1)), float(s.get("offset", 0)), s.get("unit", ""),
                                  _conf(s, where), str(s.get("evidence", ""))))
    return Profile(path, meta, bus, diag, bc)


def to_markdown(p: Profile) -> str:
    rows = ["| name | request | bytes | formula | unit | confidence | evidence |", "|---|---|---|---|---|---|---|"]
    for s in p.diag:
        f = "raw" if s.scale is None else f"raw × {s.scale:g} {s.offset:+g}"
        rows.append(f"| {s.name} | `{s.request.hex(' ').upper()}` | {s.byte}+{s.length} {s.endian} | {f} | "
                    f"{s.unit} | {s.confidence} | {s.evidence} |")
    return "\n".join(rows)


def to_dbc(p: Profile) -> str:
    """DBC for periodic (broadcast) signals. Request/response values have no fixed frame, so they stay in YAML."""
    lines = ['VERSION ""', "", "NS_ :", "", "BS_:", "", "BU_: ECU", ""]
    by_id: dict[int, list[BroadcastSignal]] = {}
    for s in p.broadcast:
        by_id.setdefault(s.can_id, []).append(s)
    for cid, sigs in sorted(by_id.items()):
        lines.append(f"BO_ {cid} MSG_{cid:03X}: 8 ECU")
        for s in sigs:
            if s.endian == "little":
                start, order = s.start_byte * 8, 1
            else:  # Motorola: start bit = MSB of first byte
                start, order = s.start_byte * 8 + 7, 0
            lines.append(f' SG_ {s.name} : {start}|{s.length * 8}@{order}+ ({s.scale:g},{s.offset:g}) '
                         f'[0|0] "{s.unit}" Vector__XXX')
        lines.append("")
    for s in p.broadcast:
        lines.append(f'CM_ SG_ {s.can_id} {s.name} "{s.confidence}: {s.evidence}";')
    return "\n".join(lines) + "\n"
