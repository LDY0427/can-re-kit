"""One CSV format for everything the tool records.

Columns: t_s, kind, can_id, data, latency_ms, request
  kind = RX (raw frame) | ANS (positive answer) | NRC | TIMEOUT | PARTIAL
  data = payload hex with spaces; for ANS the full response payload
Writing raw logs first and analysing later keeps the measurement honest:
the analysis can be redone, the drive cannot.
"""
from __future__ import annotations

import csv
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Iterator, Optional

from .transport import Frame, Response

COLUMNS = ["t_s", "kind", "can_id", "data", "latency_ms", "request"]


@dataclass
class Row:
    t: float
    kind: str
    can_id: Optional[int]
    data: bytes
    latency_ms: Optional[float]
    request: bytes

    @property
    def value(self) -> bytes:
        from .transport import echo_len

        if self.kind == "RX":
            return self.data
        return self.data[echo_len(self.request):] if self.kind == "ANS" and self.request else b""


class LogWriter:
    def __init__(self, path: Path):
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._f = self.path.open("w", newline="", encoding="utf-8")
        self._w = csv.writer(self._f)
        self._w.writerow(COLUMNS)
        self.t0 = time.monotonic()

    def frame(self, f: Frame) -> None:
        kind = "ERR" if f.error else "RX"
        cid = f"{f.can_id:08X}" if f.extended else f"{f.can_id:03X}"
        self._w.writerow([f"{f.t - self.t0:.6f}", kind, cid, f.data.hex(" ").upper(), "", ""])

    def response(self, r: Response) -> None:
        kind = {"ok": "ANS", "nrc": "NRC"}.get(r.status, r.status.upper())
        cid = f"{r.rx_id:03X}" if r.rx_id is not None else ""
        lat = f"{r.latency_ms:.3f}" if r.latency_ms is not None else ""
        self._w.writerow([f"{r.t - self.t0:.6f}", kind, cid, r.payload.hex(" ").upper(), lat,
                          r.request.hex(" ").upper()])

    def close(self) -> None:
        self._f.close()

    def __enter__(self) -> "LogWriter":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


def read_log(path: Path) -> Iterator[Row]:
    with Path(path).open(newline="", encoding="utf-8") as f:
        for rec in csv.DictReader(f):
            yield Row(
                t=float(rec["t_s"]),
                kind=rec["kind"],
                can_id=int(rec["can_id"], 16) if rec["can_id"] else None,
                data=bytes.fromhex(rec["data"]) if rec["data"] else b"",
                latency_ms=float(rec["latency_ms"]) if rec["latency_ms"] else None,
                request=bytes.fromhex(rec["request"]) if rec["request"] else b"",
            )


def key(r: Row) -> str | None:
    """'21 21' for answers to that request, 'ID 120' for raw frames with that ID."""
    if r.kind == "ANS":
        return r.request.hex(" ").upper()
    if r.kind == "RX" and r.can_id is not None:
        return f"ID {r.can_id:03X}"
    return None


def answers(rows: Iterable[Row]) -> dict[str, list[Row]]:
    """Group values: answers by request ('21 21'), raw frames by ID ('ID 120')."""
    out: dict[str, list[Row]] = {}
    for r in rows:
        k = key(r)
        if k:
            out.setdefault(k, []).append(r)
    return out
