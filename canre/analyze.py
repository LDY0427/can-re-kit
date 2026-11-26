"""Find what an unknown value means by holding it against a known one.

Method (and its limits, stated plainly):
1. Time alignment. Polled values are read at different moments, so the
   reference is linearly interpolated to each candidate sample's time.
   Samples whose surrounding reference points are more than `max_gap_s`
   apart are dropped.
2. Pearson (straight-line) and Spearman (monotonic) correlation.
3. Least-squares line: physical = slope * raw + offset, with R^2 and residuals.
4. Not judged: fewer than MIN_N samples, or fewer than 3 distinct raw values.
5. Many candidates are compared at once, so a Bonferroni-corrected chance
   threshold is reported. Time series samples are not independent, so even a
   result above it is only a *candidate*. Confirm with a deliberate test
   (hold the throttle, warm the engine, ...) before calling it measured.
"""
from __future__ import annotations

import math
from dataclasses import dataclass
from statistics import NormalDist
from typing import Callable, Iterable, Optional, Sequence

import numpy as np

from .logfile import Row, answers
from .obd import decode_pid

MIN_N = 20
MIN_DISTINCT = 3


def align(ref_t, ref_v, t, max_gap_s: float = 3.0) -> tuple[np.ndarray, np.ndarray]:
    rt, rv, tt = (np.asarray(a, float) for a in (ref_t, ref_v, t))
    if len(rt) < 2 or len(tt) == 0:
        return np.full(len(tt), np.nan), np.zeros(len(tt), bool)
    order = np.argsort(rt, kind="stable")
    rt, rv = rt[order], rv[order]
    idx = np.searchsorted(rt, tt, side="right")
    inside = (idx > 0) & (idx < len(rt))
    lo, hi = np.clip(idx - 1, 0, len(rt) - 1), np.clip(idx, 0, len(rt) - 1)
    ok = (inside & (rt[hi] - rt[lo] <= max_gap_s)) | np.isin(tt, rt)
    y = np.interp(tt, rt, rv)
    y[~ok] = np.nan
    return y, ok


def pearson(x, y) -> float:
    a, b = np.asarray(x, float), np.asarray(y, float)
    if len(a) < 2 or a.std() == 0 or b.std() == 0:
        return float("nan")
    return float(np.corrcoef(a, b)[0, 1])


def _rank(x) -> np.ndarray:
    a = np.asarray(x, float)
    order = np.argsort(a, kind="mergesort")
    ranks = np.empty(len(a))
    sa = a[order]
    i = 0
    while i < len(a):
        j = i
        while j + 1 < len(a) and sa[j + 1] == sa[i]:
            j += 1
        ranks[order[i:j + 1]] = (i + j) / 2 + 1
        i = j + 1
    return ranks


def spearman(x, y) -> float:
    return pearson(_rank(x), _rank(y))


@dataclass
class Fit:
    slope: float
    offset: float
    r2: float
    resid_rms: float
    n: int


def linfit(x, y) -> Optional[Fit]:
    a, b = np.asarray(x, float), np.asarray(y, float)
    if len(a) < 2 or a.std() == 0:
        return None
    k, c = np.polyfit(a, b, 1)
    res = b - (k * a + c)
    ss = float(((b - b.mean()) ** 2).sum())
    r2 = 1 - float((res ** 2).sum()) / ss if ss > 0 else float("nan")
    return Fit(float(k), float(c), r2, float(np.sqrt((res ** 2).mean())), len(a))


def chance_threshold(n: int, m: int, alpha: float = 0.05) -> float:
    """|r| a pure-noise pair would exceed with probability alpha, corrected for m comparisons."""
    if n <= 3 or m < 1:
        return float("nan")
    z = NormalDist().inv_cdf(1 - alpha / 2 / m)
    return math.tanh(z / math.sqrt(n - 3))


# ------------------------------------------------------------------ extracting values

Extractor = Callable[[bytes], Optional[float]]
LAYOUTS: dict[str, tuple[int, str]] = {"u8": (1, "big"), "u16be": (2, "big"), "u16le": (2, "little"),
                                        "u32be": (4, "big")}


def extractor(offset: int, layout: str) -> Extractor:
    n, endian = LAYOUTS[layout]

    def f(v: bytes) -> Optional[float]:
        return float(int.from_bytes(v[offset:offset + n], endian)) if len(v) >= offset + n else None

    return f


def series(rows: Sequence[Row], fn: Extractor) -> tuple[np.ndarray, np.ndarray]:
    t, v = [], []
    for r in rows:
        x = fn(r.value)
        if x is not None:
            t.append(r.t)
            v.append(x)
    return np.asarray(t), np.asarray(v)


def parse_reference(spec: str) -> tuple[str, Extractor]:
    """'01 0B' -> standard decode; '21 21:u16be*0.25' -> layout and scale; '21 21:u16be*0.25+0'."""
    if ":" not in spec:
        req = spec.strip().upper()
        parts = req.split()
        if len(parts) == 2 and parts[0] == "01":
            pid = int(parts[1], 16)
            return req, lambda v: decode_pid(pid, v)
        raise ValueError(f"give a layout for non-standard reference: '{spec}:u16be*0.25'")
    req, rest = spec.split(":", 1)
    req = req.strip().upper()
    scale, offset = 1.0, 0.0
    layout = rest
    if "*" in rest:
        layout, tail = rest.split("*", 1)
        if "+" in tail[1:] or "-" in tail[1:]:
            i = max(tail.rfind("+"), tail.rfind("-"))
            scale, offset = float(tail[:i]), float(tail[i:])
        else:
            scale = float(tail)
    base = extractor(0, layout.strip())
    return req, lambda v: (None if (x := base(v)) is None else x * scale + offset)


@dataclass
class Candidate:
    request: str
    offset: int
    layout: str
    n: int
    r: float
    rho: float
    fit: Optional[Fit]
    monotonic: bool

    def line(self, threshold: float) -> str:
        tag = "candidate" if abs(self.r) >= threshold else "chance?"
        f = self.fit
        fit = f"ref = {f.slope:.5g} * raw {f.offset:+.5g} (R2 {f.r2:.3f})" if f else "-"
        mono = "  [counter-like]" if self.monotonic else ""
        return (f"{self.request:<8} byte {self.offset} {self.layout:<6} n={self.n:<5} r={self.r:+.3f} "
                f"rho={self.rho:+.3f}  {fit}  {tag}{mono}")


def find_candidates(rows: Iterable[Row], reference: str, max_gap_s: float = 3.0,
                    layouts: Sequence[str] = ("u8", "u16be", "u16le")) -> tuple[list[Candidate], int, float]:
    """Hold every byte window of every other answered request against the reference."""
    groups = answers(rows)
    ref_req, ref_fn = parse_reference(reference)
    if ref_req not in groups:
        raise ValueError(f"reference request {ref_req} has no answers in this log")
    rt, rv = series(groups[ref_req], ref_fn)
    out: list[Candidate] = []
    for req, rs in groups.items():
        if req == ref_req:
            continue
        width = max(len(r.value) for r in rs)
        for layout in layouts:
            n_bytes = LAYOUTS[layout][0]
            for off in range(0, max(0, width - n_bytes + 1)):
                t, v = series(rs, extractor(off, layout))
                if len(v) < MIN_N or len(set(v.tolist())) < MIN_DISTINCT:
                    continue
                y, ok = align(rt, rv, t, max_gap_s)
                x, y = v[ok], y[ok]
                if len(x) < MIN_N:
                    continue
                mono = bool(np.all(np.diff(v) >= 0)) and v[-1] > v[0]
                out.append(Candidate(req, off, layout, len(x), pearson(x, y), spearman(x, y), linfit(x, y), mono))
    out.sort(key=lambda c: -abs(c.r) if not math.isnan(c.r) else 0)
    n_typ = int(np.median([c.n for c in out])) if out else 0
    return out, len(out), chance_threshold(n_typ, max(1, len(out)))
