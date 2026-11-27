"""canre - read-only CAN reverse-engineering helper.

Run `canre --help` or `canre <command> --help`. Try everything first with
`--interface sim` (a simulated motorcycle ECU) before touching a vehicle.
"""
from __future__ import annotations

import argparse
import statistics
import sys
import time
from datetime import datetime
from pathlib import Path

from . import __version__
from .safety import Refused, parse_hex_bytes

LOG_DIR = Path("logs")


# ------------------------------------------------------------------ connection

class Conn:
    def __init__(self, args, bitrate_kbps: int | None = None):
        from .transport import CanTransport, Elm327Transport

        self.ecu = None
        kbps = bitrate_kbps or args.bitrate
        tx_id = int(args.tx_id, 16)
        allowed = frozenset(int(x, 16) for x in args.allow_tx_id)
        if args.interface == "esp32":
            from .transport import SctTransport

            if not args.channel:
                sys.exit("--channel is the serial port of the ESP32-S3 running firmware/, e.g. COM9")
            self.t = SctTransport(args.channel, bitrate_kbps=kbps, tx_id=tx_id, allowed_tx=allowed, force=args.force)
            return
        if args.interface == "elm327":
            if not args.channel:
                sys.exit("--channel is the serial port of the ELM327, e.g. COM5 or /dev/rfcomm0")
            self.t = Elm327Transport(args.channel, bitrate_kbps=kbps, tx_id=tx_id, allowed_tx=allowed)
            return
        import can

        if args.interface == "sim":
            from .sim import SimEcu

            ch = f"canre-sim-{id(self)}"
            self.ecu = SimEcu(ch, broadcast=args.sim_broadcast).start()
            bus = can.Bus(interface="virtual", channel=ch, receive_own_messages=False)
        else:
            kw = {"interface": args.interface, "channel": args.channel, "bitrate": kbps * 1000}
            if getattr(args, "listen_only", False):
                if args.interface in ("slcan", "pcan", "kvaser"):
                    kw["listen_only"] = True
                else:
                    print(f"! cannot switch {args.interface} to listen-only from here; it will ACK frames. "
                          "(SocketCAN: 'ip link set can0 type can bitrate ... listen-only on')", file=sys.stderr)
            bus = can.Bus(**{k: v for k, v in kw.items() if v is not None})
        self.t = CanTransport(bus, tx_id=tx_id, allowed_tx=allowed, force=args.force)

    def close(self) -> None:
        self.t.close()
        if self.ecu:
            self.ecu.stop()


def _log_path(args, name: str) -> Path:
    if getattr(args, "log", None):
        return Path(args.log)
    return LOG_DIR / f"{name}_{datetime.now():%Y%m%d_%H%M%S}.csv"


# ------------------------------------------------------------------ commands

def cmd_listen(args) -> int:
    from .logfile import LogWriter
    from .sniff import summarise

    args.listen_only = not args.ack
    c = Conn(args)
    path = _log_path(args, "listen")
    frames = []
    try:
        with LogWriter(path) as log:
            for f in c.t.listen(args.seconds):
                log.frame(f)
                frames.append(f)
    finally:
        c.close()
    print(summarise(frames, args.seconds).table())
    print(f"log: {path}")
    return 0


def cmd_bitrate(args) -> int:
    from .sniff import summarise

    if args.interface in ("sim", "elm327", "esp32"):
        sys.exit("bitrate search needs a real CAN adapter (python-can interface)")
    args.listen_only = True
    for kbps in args.rates:
        c = Conn(args, bitrate_kbps=kbps)
        try:
            s = summarise(c.t.listen(args.seconds), args.seconds)
        finally:
            c.close()
        print(f"{kbps:5d} kbps: {s.frames} frames, {s.errors} errors, {len(s.ids)} IDs")
    print("A rate with frames and no errors is the one. A quiet bus shows nothing at any rate: then use 'ping'.")
    return 0


def _print_response(r) -> None:
    print(f"{r.request.hex(' ').upper():<12} -> {r.describe()}")


def cmd_ping(args) -> int:
    c = Conn(args)
    try:
        r = c.t.request(bytes([0x01, 0x00]), timeout=0.3)
    finally:
        c.close()
    _print_response(r)
    if r.status == "ok":
        print("The ECU answered: bitrate and wiring are right.")
        return 0
    print("No answer. Check: key ON, bitrate (try --bitrate 250), CAN-H/L swapped, termination, ground.")
    return 1


def cmd_req(args) -> int:
    from .logfile import LogWriter

    from .safety import check_request

    req = parse_hex_bytes(args.bytes)
    check_request(req)  # refuse before opening the bus
    c = Conn(args)
    try:
        with LogWriter(_log_path(args, "req")) as log:
            for _ in range(args.repeat):
                r = c.t.request(req, timeout=args.timeout)
                log.response(r)
                _print_response(r)
    finally:
        c.close()
    return 0


def cmd_pids(args) -> int:
    from .obd import PIDS, SUPPORT_PIDS, decode_pid, supported_from_bitmap

    c = Conn(args)
    supported: list[int] = []
    try:
        for base in SUPPORT_PIDS:
            r = c.t.request(bytes([0x01, base]), timeout=args.timeout)
            if r.status != "ok":
                break
            got = supported_from_bitmap(base, r.value)
            supported += [p for p in got if p % 0x20]
            if base + 0x20 not in got:
                break
        print("supported 01 PIDs:", " ".join(f"{p:02X}" for p in supported) or "(none)")
        for p in supported:
            r = c.t.request(bytes([0x01, p]), timeout=args.timeout)
            v = decode_pid(p, r.value) if r.status == "ok" else None
            info = PIDS.get(p)
            val = f"{v:.2f} {info.unit}" if v is not None else r.describe()
            print(f"  01 {p:02X}  {info.name if info else '?':<18} {val}")
        if 0x0C not in supported:
            print("note: standard RPM (01 0C) not supported. Common on motorcycles; look in manufacturer data.")
    finally:
        c.close()
    return 0


def _read_dtcs(t, timeout: float) -> dict[str, list[str]]:
    from .obd import decode_dtcs

    out = {}
    for sid, name in ((0x03, "stored"), (0x07, "pending"), (0x0A, "permanent")):
        r = t.request(bytes([sid]), timeout=timeout)
        out[name] = decode_dtcs(r.payload)[0] if r.status == "ok" else [f"({r.status})"]
    return out


def cmd_dtc(args) -> int:
    c = Conn(args)
    try:
        for k, v in _read_dtcs(c.t, args.timeout).items():
            print(f"{k:<10} {' '.join(v) or '-'}")
    finally:
        c.close()
    print("Read only. This tool cannot clear codes. Write the codes down before and after every session.")
    return 0


def cmd_scan(args) -> int:
    from .logfile import LogWriter

    if not args.engine_off:
        sys.exit("scan sends many requests in a row. Do it with key ON and engine OFF, then pass --engine-off.\n"
                 "Read docs/04-workflow.md (step 5) first.")
    sid = int(args.service, 16)
    if sid not in (0x21, 0x22):
        sys.exit("scan supports service 21 (1-byte local IDs) and 22 (2-byte identifiers)")
    lo, hi = int(args.start, 16), int(args.end, 16)
    c = Conn(args)
    found = 0
    try:
        before = _read_dtcs(c.t, args.timeout)
        print("DTCs before:", before)
        with LogWriter(_log_path(args, f"scan{sid:02X}")) as log:
            for i, ident in enumerate(range(lo, hi + 1)):
                req = bytes([sid, ident]) if sid == 0x21 else bytes([sid, ident >> 8, ident & 0xFF])
                r = c.t.request(req, timeout=args.timeout)
                log.response(r)
                if r.status == "ok":
                    found += 1
                    print(f"{req.hex(' ').upper():<10} {len(r.value):2d} B  {r.value.hex(' ').upper()}")
                elif args.verbose:
                    _print_response(r)
                if args.check_every and (i + 1) % args.check_every == 0:
                    now = _read_dtcs(c.t, args.timeout)
                    if now != before:
                        print("DTCs changed during scan, stopping:", now)
                        return 2
                time.sleep(args.gap / 1000)
        after = _read_dtcs(c.t, args.timeout)
        print(f"{found} identifiers answered. DTCs after:", after)
        if after != before:
            print("! DTCs changed. Note it in your log; see docs/04-workflow.md.")
    finally:
        c.close()
    return 0


def cmd_poll(args) -> int:
    from .logfile import LogWriter
    from .profile import load

    reqs = [parse_hex_bytes(r) for r in args.requests]
    prof = load(args.profile) if args.profile else None
    decoders = {}
    if prof:
        for s in prof.diag:
            decoders.setdefault(s.request, []).append(s)
    c = Conn(args)
    stats = {r: [] for r in reqs}
    fails = {r: 0 for r in reqs}
    path = _log_path(args, "poll")
    try:
        with LogWriter(path) as log:
            if args.record_all and hasattr(c.t, "sink"):
                c.t.sink = log.frame  # broadcast frames go in the same log, for 'analyze'
            for n in range(args.count):
                t_next = time.monotonic() + args.interval / 1000
                cells = []
                for req in reqs:
                    r = c.t.request(req, timeout=args.timeout)
                    log.response(r)
                    if r.status == "ok":
                        stats[req].append(r.latency_ms)
                        decoded = [f"{s.name}={s.decode(r.value):.6g}{s.unit}" for s in decoders.get(req, [])]
                        cells.append(" ".join(decoded) or r.value.hex(" ").upper())
                    else:
                        fails[req] += 1
                        cells.append(r.status)
                if not args.quiet:
                    print(f"{n:5d}  " + " | ".join(cells))
                time.sleep(max(0.0, t_next - time.monotonic()))
    finally:
        c.close()
    print("\nrequest      ok/total  latency ms (median / max)")
    for req in reqs:
        lat = stats[req]
        med = f"{statistics.median(lat):.1f} / {max(lat):.1f}" if lat else "-"
        print(f"{req.hex(' ').upper():<12} {len(lat)}/{len(lat) + fails[req]:<6}  {med}")
    print(f"log: {path}")
    return 0


def cmd_analyze(args) -> int:
    from .analyze import find_candidates
    from .logfile import read_log

    rows = list(read_log(args.log_file))
    cands, m, thr = find_candidates(rows, args.reference, max_gap_s=args.max_gap)
    print(f"reference {args.reference}; {m} byte windows compared; chance threshold |r| > {thr:.3f} (Bonferroni)")
    for cnd in cands[: args.top]:
        print(cnd.line(thr))
    print("These are candidates. Confirm each with a deliberate test before marking it 'measured'.")
    return 0


def cmd_profile(args) -> int:
    from .profile import ProfileError, load, to_dbc, to_markdown

    try:
        p = load(args.file)
    except ProfileError as e:
        print(f"invalid: {e}")
        return 1
    if args.action == "check":
        counts = {}
        for s in [*p.diag, *p.broadcast]:
            counts[s.confidence] = counts.get(s.confidence, 0) + 1
        print(f"ok: {p.meta.get('make', '')} {p.meta.get('model', '')} {p.meta.get('year', '')} - "
              + ", ".join(f"{k} {v}" for k, v in counts.items()))
    elif args.action == "table":
        print(to_markdown(p))
    elif args.action == "dbc":
        print(to_dbc(p), end="")
    return 0


# ------------------------------------------------------------------ parser

def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog="canre", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", action="version", version=f"canre {__version__}")
    conn = argparse.ArgumentParser(add_help=False)
    g = conn.add_argument_group("connection")
    g.add_argument("-i", "--interface", default="sim",
                   help="python-can interface (slcan, gs_usb, pcan, socketcan, kvaser, vector...), "
                        "'esp32' (firmware/ in this repo), 'elm327', or 'sim' (default)")
    g.add_argument("-c", "--channel", help="e.g. COM7, can0, /dev/ttyACM0")
    g.add_argument("-b", "--bitrate", type=int, default=500, help="kbps (default 500)")
    g.add_argument("--tx-id", default="7DF", help="request ID, hex (default 7DF; 7E0 = engine ECU direct)")
    g.add_argument("--allow-tx-id", action="append", default=[], metavar="HEX",
                   help="allow a non-OBD request ID you have confirmed")
    g.add_argument("--timeout", type=float, default=0.2, help="seconds to wait for an answer")
    g.add_argument("--force", action="store_true", help="send even if bus errors were seen before sending")
    g.add_argument("--sim-broadcast", action="store_true", help="sim: also send a periodic 0x120 frame")
    g.add_argument("--log", help="CSV log path (default logs/<command>_<time>.csv)")

    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("listen", parents=[conn], help="record raw traffic and summarise IDs")
    p.add_argument("-s", "--seconds", type=float, default=30)
    p.add_argument("--ack", action="store_true", help="normal mode (ACK frames) instead of listen-only")
    p.set_defaults(fn=cmd_listen)

    p = sub.add_parser("bitrate", parents=[conn], help="listen at several bitrates")
    p.add_argument("--rates", type=int, nargs="+", default=[500, 250, 125, 1000])
    p.add_argument("-s", "--seconds", type=float, default=5)
    p.set_defaults(fn=cmd_bitrate)

    p = sub.add_parser("ping", parents=[conn], help="send one '01 00' and see if an ECU answers")
    p.set_defaults(fn=cmd_ping)

    p = sub.add_parser("req", parents=[conn], help="send one read request, e.g. 'req 21 21'")
    p.add_argument("bytes", nargs="+", help="request bytes without the length byte")
    p.add_argument("-n", "--repeat", type=int, default=1)
    p.set_defaults(fn=cmd_req)

    p = sub.add_parser("pids", parents=[conn], help="list and read supported standard PIDs")
    p.set_defaults(fn=cmd_pids)

    p = sub.add_parser("dtc", parents=[conn], help="read stored/pending/permanent DTCs (never clears)")
    p.set_defaults(fn=cmd_dtc)

    p = sub.add_parser("scan", parents=[conn], help="try every identifier of service 21 or 22 (read only)")
    p.add_argument("--service", default="21")
    p.add_argument("--start", default="00")
    p.add_argument("--end", default="FF")
    p.add_argument("--gap", type=float, default=20, help="ms between requests (default 20)")
    p.add_argument("--check-every", type=int, default=16, help="re-read DTCs every N requests (0 = off)")
    p.add_argument("--engine-off", action="store_true", help="confirm: key ON, engine OFF, vehicle secured")
    p.add_argument("-v", "--verbose", action="store_true")
    p.set_defaults(fn=cmd_scan)

    p = sub.add_parser("poll", parents=[conn], help="repeat requests and log them, e.g. poll '21 21' '01 05'")
    p.add_argument("requests", nargs="+", help="each request in quotes")
    p.add_argument("--interval", type=float, default=100, help="ms per round (default 100)")
    p.add_argument("-n", "--count", type=int, default=100)
    p.add_argument("--profile", help="decode with a vehicle profile YAML")
    p.add_argument("--record-all", action="store_true",
                   help="also log every other frame seen (to correlate broadcast bytes with polled values)")
    p.add_argument("-q", "--quiet", action="store_true")
    p.set_defaults(fn=cmd_poll)

    p = sub.add_parser("analyze", help="rank unknown bytes in a poll log against a known reference")
    p.add_argument("log_file")
    p.add_argument("--reference", required=True,
                   help="'01 0B' (standard PID), '21 21:u16be*0.25' (request:layout*scale+offset) "
                        "or 'ID 120:u16be' (a broadcast frame)")
    p.add_argument("--max-gap", type=float, default=3.0, help="s, max gap when aligning in time")
    p.add_argument("--top", type=int, default=15)
    p.set_defaults(fn=cmd_analyze)

    p = sub.add_parser("profile", help="check a vehicle profile, print it as a table, or export DBC")
    p.add_argument("action", choices=["check", "table", "dbc"])
    p.add_argument("file")
    p.set_defaults(fn=cmd_profile)
    return ap


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.fn(args)
    except Refused as e:
        print(f"refused: {e}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
