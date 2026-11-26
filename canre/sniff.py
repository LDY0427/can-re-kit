"""Summarise raw traffic: which IDs exist, how often, which bytes move.

The byte-change map is the first clue in broadcast reverse engineering:
a byte that never changes is a constant, one that changes every frame is a
counter or checksum, one that follows what you do (throttle, brake) is a signal.
"""
from __future__ import annotations

import statistics
from dataclasses import dataclass, field
from typing import Iterable

from .transport import Frame


@dataclass
class IdStats:
    can_id: int
    extended: bool
    count: int = 0
    dlcs: set[int] = field(default_factory=set)
    times: list[float] = field(default_factory=list)
    first: bytes = b""
    last: bytes = b""
    changed: list[int] = field(default_factory=lambda: [0] * 8)  # how often each byte changed
    distinct: list[set[int]] = field(default_factory=lambda: [set() for _ in range(8)])

    def add(self, f: Frame) -> None:
        if self.count == 0:
            self.first = f.data
        else:
            for i, b in enumerate(f.data[:8]):
                if i < len(self.last) and self.last[i] != b:
                    self.changed[i] += 1
        for i, b in enumerate(f.data[:8]):
            self.distinct[i].add(b)
        self.last = f.data
        self.count += 1
        self.dlcs.add(len(f.data))
        self.times.append(f.t)

    def period_ms(self) -> tuple[float, float, float] | None:
        if len(self.times) < 2:
            return None
        d = [(b - a) * 1000 for a, b in zip(self.times, self.times[1:])]
        return statistics.mean(d), min(d), max(d)

    def byte_map(self) -> str:
        """One char per byte: '.' constant, 'c' changes nearly every frame, '*' changes sometimes."""
        n = max(self.dlcs) if self.dlcs else 0
        out = []
        for i in range(n):
            if len(self.distinct[i]) <= 1:
                out.append(".")
            elif self.count > 2 and self.changed[i] >= 0.9 * (self.count - 1):
                out.append("c")
            else:
                out.append("*")
        return "".join(out)


@dataclass
class Summary:
    ids: dict[int, IdStats]
    frames: int
    errors: int
    seconds: float

    def table(self) -> str:
        lines = [f"{self.frames} frames, {self.errors} error frames, {len(self.ids)} IDs in {self.seconds:.1f} s",
                 "ID        count  dlc  period ms (mean/min/max)   bytes     last data"]
        for s in sorted(self.ids.values(), key=lambda s: s.can_id):
            cid = f"{s.can_id:08X}" if s.extended else f"{s.can_id:03X}"
            p = s.period_ms()
            per = f"{p[0]:7.1f} /{p[1]:6.1f} /{p[2]:7.1f}" if p else " " * 23
            lines.append(f"{cid:<8} {s.count:6d}  {','.join(map(str, sorted(s.dlcs))):<4} {per}   "
                         f"{s.byte_map():<8}  {s.last.hex(' ').upper()}")
        if not self.ids:
            lines.append("(no frames: the bus is quiet, or bitrate/wiring is wrong; see docs/04)")
        else:
            lines.append("bytes: '.' constant  '*' changes sometimes  'c' changes almost every frame (counter/checksum?)")
        return "\n".join(lines)


def summarise(frames: Iterable[Frame], seconds: float) -> Summary:
    ids: dict[int, IdStats] = {}
    n = err = 0
    for f in frames:
        if f.error:
            err += 1
            continue
        n += 1
        ids.setdefault(f.can_id, IdStats(f.can_id, f.extended)).add(f)
    return Summary(ids, n, err, seconds)
