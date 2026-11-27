"""ISO-TP 시험 벡터(isotp_vectors.json)를 만든다. 긴 경우(순번 넘김, 4,095바이트)를 손으로 적지 않으려고 둔 스크립트다.

사용: python test/vectors/gen_isotp_vectors.py   (같은 폴더에 isotp_vectors.json을 다시 쓴다)

벡터를 읽는 쪽(C: test/unit/test_isotp.c, 파이썬: pc/ 쪽)의 규칙
  - 수신 ID 범위 7E8~7EF, 요청과의 짝짓기 없이(모든 SF·FF를 받는다) 프레임을 차례로 넣는다.
  - 프레임의 DLC = data의 바이트 수. 시각 = t_ms.
  - 응답이 끝나거나(ok) 깨지면(wrong_sn, timeout_cr, bad_len) 그 경우는 끝이다. 뒤 프레임은 넣지 않는다.
  - 끝나지 않았고 end_t_ms가 있으면 그 시각으로 시간만 흘린다(N_Cr 1,000 ms 확인).
  - status none = 응답으로 친 것이 없다(모두 버렸거나 남의 것).
  - fc = 흐름 제어를 보내라고 한 것 전부: after_frame = 그 요청을 낸 프레임의 순번(0부터).
  - payload·total: ok면 응답 전체와 길이, 깨졌으면 받은 만큼과 FF가 알린 전체 길이.
"""
import json
import os

FC = "30 00 00 00 00 00 00 00"
PAD = 0xAA


def hx(b):
    return " ".join("%02X" % x for x in b)


def multi(payload, rid=0x7E8, t0=0, step=1, pad=True):
    """payload를 FF + CF들로. 프레임 목록을 돌려준다."""
    n = len(payload)
    frames = [{"t_ms": t0, "id": "%03X" % rid, "data": hx([0x10 | (n >> 8), n & 0xFF] + list(payload[:6]))}]
    off, sn, t = 6, 1, t0
    while off < n:
        t += step
        chunk = list(payload[off:off + 7])
        if pad:
            chunk += [PAD] * (7 - len(chunk))
        frames.append({"t_ms": t, "id": "%03X" % rid, "data": hx([0x20 | sn] + chunk)})
        off += 7
        sn = (sn + 1) & 0x0F
    return frames


def sf(payload, rid=0x7E8, t=0, pad=True):
    d = [len(payload)] + list(payload)
    if pad:
        d += [PAD] * (8 - len(d))
    return {"t_ms": t, "id": "%03X" % rid, "data": hx(d)}


def fc(after, rid=0x7E8):
    return {"after_frame": after, "id": "%03X" % (rid - 8), "data": FC}


def ok(payload, fcs):
    return {"status": "ok", "payload": hx(payload), "total": len(payload), "fc": fcs}


DTC03 = bytes.fromhex("430C065015061502150516060118160104800030011316070117")
DTC07 = bytes.fromhex("470515020000048000300000")      # [실측] 이번 주행 코드 3개, 12바이트
DTC07_14 = bytes.fromhex("4706150200001505048000300000")  # [실측] P1505가 더해진 뒤, 14바이트
SEQ125 = bytes(i & 0xFF for i in range(125))
SEQ20 = bytes(range(0x30, 0x30 + 20))
SEQ4095 = bytes((i * 7 + 3) & 0xFF for i in range(4095))

cases = []
cases.append({"name": "sf_single", "note": "한 프레임 응답(01 05 수온, 실측 값 54)",
              "frames": [sf(b"\x41\x05\x54")], "expect": ok(b"\x41\x05\x54", [])})
# 아래 세 경우의 프레임은 ELM327 통신 기록 원본(11-17, 500 kbps, 7DF→7E8)의 바이트 그대로다(AA 채움 포함). [실측]
def raw(*datas):
    return [{"t_ms": i, "id": "7E8", "data": d} for i, d in enumerate(datas)]


cases.append({"name": "mf_dtc03_26_measured", "note": "실측 03 응답 26바이트(저장 코드 12개): FF + CF 3개, 원본 프레임 그대로",
              "frames": raw("10 1A 43 0C 06 50 15 06", "21 15 02 15 05 16 06 01", "22 18 16 01 04 80 00 30", "23 01 13 16 07 01 17 AA"),
              "expect": ok(DTC03, [fc(0)])})
cases.append({"name": "mf_dtc07_12_measured", "note": "실측 07 응답 12바이트(이번 주행 코드 3개, 0000 채움 2쌍): FF + CF 1개, 원본 프레임 그대로",
              "frames": raw("10 0C 47 05 15 02 00 00", "21 04 80 00 30 00 00 AA"),
              "expect": ok(DTC07, [fc(0)])})
cases.append({"name": "mf_dtc07_14_measured", "note": "실측 07 응답 14바이트(P1505가 더해진 뒤): FF + CF 2개, 원본 프레임 그대로",
              "frames": raw("10 0E 47 06 15 02 00 00", "21 15 05 04 80 00 30 00", "22 00 AA AA AA AA AA AA"),
              "expect": ok(DTC07_14, [fc(0)])})
cases.append({"name": "sn_wrap_15_to_0", "note": "125바이트: CF 17개, 순번 1..15 다음 0, 1",
              "frames": multi(SEQ125), "expect": ok(SEQ125, [fc(0)])})
f = multi(SEQ20)
f[2]["data"] = "23" + f[2]["data"][2:]
cases.append({"name": "wrong_sn", "note": "둘째 CF의 순번이 2가 아니라 3",
              "frames": f, "expect": {"status": "wrong_sn", "payload": hx(SEQ20[:13]), "total": 20, "fc": [fc(0)]}})
f = multi(SEQ20)[:2]
cases.append({"name": "timeout_cr", "note": "CF 하나 뒤 끊김. 마지막 CF 뒤 1,000 ms(N_Cr)가 지남",
              "frames": f, "end_t_ms": f[-1]["t_ms"] + 1000,
              "expect": {"status": "timeout_cr", "payload": hx(SEQ20[:13]), "total": 20, "fc": [fc(0)]}})
f = multi(SEQ20)[:2]
cases.append({"name": "no_timeout_before_n_cr", "note": "CF 하나 뒤 999 ms: 아직 받는 중(응답으로 치지 않음)",
              "frames": f, "end_t_ms": f[-1]["t_ms"] + 999, "expect": {"status": "none", "payload": "", "total": 0, "fc": [fc(0)]}})
cases.append({"name": "ff_len_below_8", "note": "FF 길이 5: SF로 보냈어야 한다. 버림",
              "frames": [{"t_ms": 0, "id": "7E8", "data": "10 05 41 05 54 00 00 00"}], "expect": {"status": "none", "payload": "", "total": 0, "fc": []}})
cases.append({"name": "ff_len_zero", "note": "FF 길이 0 = 32비트 길이 형식. 지원하지 않아 버림",
              "frames": [{"t_ms": 0, "id": "7E8", "data": "10 00 00 00 10 00 41 05"}], "expect": {"status": "none", "payload": "", "total": 0, "fc": []}})
cases.append({"name": "sf_dlc_short", "note": "SF 길이 3인데 DLC 3(4 이상이어야). 버림",
              "frames": [{"t_ms": 0, "id": "7E8", "data": "03 41 05"}], "expect": {"status": "none", "payload": "", "total": 0, "fc": []}})
cases.append({"name": "ff_dlc_short", "note": "FF의 DLC 5(8이어야). 버림",
              "frames": [{"t_ms": 0, "id": "7E8", "data": "10 0A 41 00 01"}], "expect": {"status": "none", "payload": "", "total": 0, "fc": []}})
f = multi(bytes(range(1, 11)))
f[1]["data"] = "21 07 08"
cases.append({"name": "cf_dlc_short", "note": "10바이트 응답의 CF가 4바이트를 실어야 하는데 DLC 3. 받은 8바이트로 끝냄",
              "frames": f, "expect": {"status": "bad_len", "payload": hx(bytes(range(1, 9))), "total": 10, "fc": [fc(0)]}})
f = multi(SEQ20)[:2] + [dict(x, t_ms=x["t_ms"] + 10) for x in multi(DTC07)]
cases.append({"name": "new_ff_during_rx", "note": "받는 중 같은 ID의 새 FF: 앞을 버리고 새로 받는다",
              "frames": f, "expect": ok(DTC07, [fc(0), fc(2)])})
f = multi(SEQ20)[:2] + [sf(b"\x41\x0D\x00", t=5)]
cases.append({"name": "sf_during_rx", "note": "받는 중 같은 ID의 SF: 앞을 버리고 SF를 응답으로",
              "frames": f, "expect": ok(b"\x41\x0D\x00", [fc(0)])})
f = multi(DTC07)
other = multi(SEQ20, rid=0x7E9, t0=0)
f = [f[0], dict(other[0], t_ms=0), dict(f[1], t_ms=1)]
cases.append({"name": "other_id_ff_during_rx", "note": "7E8을 받는 중 7E9의 FF는 무시(흐름 제어도 안 보냄)",
              "frames": f, "expect": ok(DTC07, [fc(0)])})
cases.append({"name": "response_from_7e9", "note": "7E9가 답하면 흐름 제어는 7E1로",
              "frames": multi(DTC07, rid=0x7E9), "expect": ok(DTC07, [fc(0, rid=0x7E9)])})
cases.append({"name": "max_4095", "note": "12비트 길이의 최대 4,095바이트: CF 585개, 마지막 CF는 1바이트",
              "frames": multi(SEQ4095), "expect": ok(SEQ4095, [fc(0)])})
cases.append({"name": "cf_without_ff", "note": "받는 중이 아닌데 CF: 남의 것",
              "frames": [{"t_ms": 0, "id": "7E8", "data": "21 01 02 03 04 05 06 07"}], "expect": {"status": "none", "payload": "", "total": 0, "fc": []}})
cases.append({"name": "not_response_id", "note": "응답 ID 범위 밖(280) 프레임",
              "frames": [{"t_ms": 0, "id": "280", "data": "03 41 05 54 00 00 00 00"}], "expect": {"status": "none", "payload": "", "total": 0, "fc": []}})
cases.append({"name": "sf_len_zero_and_eight", "note": "SF 길이 0과 8은 형식 위반. 버림",
              "frames": [{"t_ms": 0, "id": "7E8", "data": "00 41 05 54 00 00 00 00"}, {"t_ms": 1, "id": "7E8", "data": "08 41 05 54 00 00 00 00"}],
              "expect": {"status": "none", "payload": "", "total": 0, "fc": []}})
cases.append({"name": "fc_from_ecu_ignored", "note": "응답 ID로 온 흐름 제어(3x)는 응답 데이터가 아니다",
              "frames": [{"t_ms": 0, "id": "7E8", "data": FC}], "expect": {"status": "none", "payload": "", "total": 0, "fc": []}})
f = multi(DTC03, step=900)
cases.append({"name": "slow_cf_within_n_cr", "note": "CF 간격 900 ms: N_Cr 안이라 끝까지 받는다",
              "frames": f, "expect": ok(DTC03, [fc(0)])})
cases.append({"name": "unpadded_last_cf", "note": "마지막 CF를 패딩 없이 DLC 7로 보낸 경우도 받는다",
              "frames": multi(DTC03, pad=False), "expect": ok(DTC03, [fc(0)])})

doc = {"version": 1,
       "note": "stock-can-tool v2 ISO-TP 수신 시험 벡터. 생성: gen_isotp_vectors.py. C 시험과 파이썬 시험이 같이 쓴다.",
       "cases": cases}
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "isotp_vectors.json")
with open(out, "w", encoding="utf-8", newline="\n") as fp:
    json.dump(doc, fp, ensure_ascii=False, indent=1)
    fp.write("\n")
print("%d cases -> %s" % (len(cases), out))
