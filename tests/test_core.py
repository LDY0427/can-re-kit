import math

import numpy as np
import pytest

from canre import isotp
from canre.analyze import align, chance_threshold, find_candidates, linfit, parse_reference
from canre.logfile import Row
from canre.obd import decode_dtcs, decode_pid, supported_from_bitmap
from canre.safety import Refused, check_request, check_tx_id, parse_hex_bytes


# ---------------------------------------------------------------- safety

@pytest.mark.parametrize("req", ["01 00", "03", "21 21", "22 F1 90", "09 02", "19 02 FF"])
def test_read_services_pass(req):
    check_request(parse_hex_bytes(req))


@pytest.mark.parametrize("req", ["04", "14 FF FF FF", "27 01", "2E F1 90 00", "31 01 02 03", "11 01",
                                 "10 02", "34 00", "3D 00", "85 02", "2F 01 00"])
def test_write_and_control_services_refused(req):
    with pytest.raises(Refused):
        check_request(parse_hex_bytes(req))


def test_request_length_limits():
    with pytest.raises(Refused):
        check_request(b"")
    with pytest.raises(Refused):
        check_request(bytes([0x22] + [0] * 7))


def test_tx_id_policy():
    check_tx_id(0x7DF)
    check_tx_id(0x7E0)
    with pytest.raises(Refused):
        check_tx_id(0x123)
    check_tx_id(0x123, frozenset({0x123}))


def test_parse_hex():
    assert parse_hex_bytes("21 21") == b"\x21\x21"
    assert parse_hex_bytes("2121") == b"\x21\x21"
    assert parse_hex_bytes(["21", "0x3b"]) == b"\x21\x3b"
    with pytest.raises(ValueError):
        parse_hex_bytes("123")


# ---------------------------------------------------------------- iso-tp

@pytest.mark.parametrize("n", [1, 6, 7, 8, 13, 14, 20, 62])
def test_isotp_roundtrip(n):
    payload = bytes(range(n))
    r = isotp.Reassembler()
    out = None
    for f in isotp.segment(payload):
        assert len(f) == 8
        out = r.feed(f) or out
    assert out == payload


def test_isotp_bad_sequence_resets():
    frames = isotp.segment(bytes(range(20)))
    r = isotp.Reassembler()
    r.feed(frames[0])
    assert r.feed(frames[2]) is None
    assert r.error and not r.in_progress


# ---------------------------------------------------------------- obd

def test_supported_bitmap():
    # 41 00 08 2E 80 00 -> 05 0B 0D 0E 0F 11
    assert supported_from_bitmap(0, bytes.fromhex("082E8000")) == [0x05, 0x0B, 0x0D, 0x0E, 0x0F, 0x11]


def test_decode_pid():
    assert decode_pid(0x05, b"\x5A") == 50
    assert decode_pid(0x0C, b"\x1A\xF8") == 1726
    assert decode_pid(0x0C, b"\x1A") is None


def test_dtc_counts_padding_pairs():
    # A real ECU answer: the count byte includes the 0000 padding pairs.
    codes, mismatch = decode_dtcs(bytes.fromhex("4706150200001505048000300000"))
    assert codes == ["P1502", "P1505", "P0480", "P0030"] and not mismatch


def test_dtc_truncated_answer_flagged():
    codes, mismatch = decode_dtcs(bytes.fromhex("43050650"))
    assert codes == ["P0650"] and mismatch


# ---------------------------------------------------------------- analysis

def _rows(req, ts, raws, width):
    r = bytes.fromhex(req.replace(" ", ""))
    return [Row(t, "ANS", 0x7E8, bytes([r[0] + 0x40, *r[1:]]) + int(v).to_bytes(width, "big"), 1.0, r)
            for t, v in zip(ts, raws)]


def test_find_candidates_recovers_scale():
    rng = np.random.default_rng(1)
    t_ref = np.arange(0, 60, 0.1)
    throttle = 50 + 45 * np.sin(t_ref / 3)
    ref_raw = np.round(throttle * 255 / 100)
    t_c = t_ref + 0.05
    unknown = np.round(680 + 30 * (50 + 45 * np.sin(t_c / 3)))
    noise = rng.integers(0, 255, len(t_c))
    rows = _rows("01 11", t_ref, ref_raw, 1) + _rows("21 3B", t_c, unknown, 2) + _rows("21 99", t_c, noise, 1)
    cands, m, thr = find_candidates(rows, "01 11")
    best = cands[0]
    assert (best.request, best.offset, best.layout) == ("21 3B", 0, "u16be")
    assert best.fit.slope == pytest.approx(1 / 30, rel=0.02)
    assert best.fit.offset == pytest.approx(-680 / 30, abs=1.0)
    noisy = next(c for c in cands if c.request == "21 99")
    assert abs(noisy.r) < thr


def test_align_drops_wide_gaps():
    y, ok = align([0, 1, 10, 11], [0, 1, 10, 11], [0.5, 5.0, 10.5], max_gap_s=3)
    assert ok.tolist() == [True, False, True]
    assert y[0] == pytest.approx(0.5)


def test_reference_spec_parsing():
    req, fn = parse_reference("21 21:u16be*0.25")
    assert req == "21 21" and fn(b"\x1A\xF8") == pytest.approx(1726)
    req, fn = parse_reference("21 22:u8*0.75-48")
    assert fn(b"\x68") == pytest.approx(0.75 * 0x68 - 48)
    with pytest.raises(ValueError):
        parse_reference("21 22")


def test_linfit_and_threshold():
    f = linfit([0, 1, 2, 3], [1, 3, 5, 7])
    assert f.slope == pytest.approx(2) and f.offset == pytest.approx(1) and f.r2 == pytest.approx(1)
    assert 0 < chance_threshold(400, 20) < chance_threshold(50, 20) < 1
    assert math.isnan(chance_threshold(3, 1))
