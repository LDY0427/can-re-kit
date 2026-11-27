"""PC(WebAssembly) 빌드와 단위 시험.

사용:
  python test/host_build.py              # tests와 sim을 빌드하고 tests를 돌린다
  python test/host_build.py tests        # 단위 시험만 빌드·실행
  python test/host_build.py sim          # 대화형 가상 펌웨어(test/build/sim.wasm)만 빌드
  python test/host_build.py tests --update-golden   # 골든 출력(test/golden/*.out)을 다시 쓴다. 내용을 사람이 보고 커밋한다

Windows의 Smart App Control이 새로 만든 .exe를 막을 수 있어서 네이티브 실행 파일을 만들지 않고 wasm32-wasi로
빌드해 Node.js(node:wasi)로 돌린다. 컴파일러는 파이썬 패키지 ziglang(0.13.0)의 `zig cc`다.
종료 코드: 0 = 빌드·시험 성공, 1 = 시험 실패, 2 = 빌드 실패.
"""
import glob
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(HERE, "build")
CORE_SRC = sorted(glob.glob(os.path.join(ROOT, "components", "sct_core", "src", "*.c")))
SIM_SRC = [os.path.join(ROOT, "port_sim", f) for f in ("sim_bus.c", "sim_ecu.c")]
CFLAGS = ["-target", "wasm32-wasi", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
          "-DSCT_BUILD_DATE=\"sim\"",  # 시험 출력이 날마다 바뀌지 않게 빌드 날짜를 고정
          "-I" + os.path.join(ROOT, "components", "sct_core", "include"), "-I" + os.path.join(ROOT, "port_sim")]


def zig_cc(args):
    cmd = [sys.executable, "-m", "ziglang", "cc"] + CFLAGS + args
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0 or r.stderr.strip():
        sys.stdout.write(r.stdout + r.stderr)
    return r.returncode == 0 and not r.stderr.strip()  # 경고도 실패로 본다(-Werror와 같게)


def gen_vectors_header():
    """test/vectors/isotp_vectors.json → test/build/isotp_vectors.h (C 배열)"""
    src = os.path.join(HERE, "vectors", "isotp_vectors.json")
    with open(src, encoding="utf-8") as f:
        doc = json.load(f)
    assert doc["version"] == 1
    out = ["// 자동 생성: test/host_build.py ← test/vectors/isotp_vectors.json. 고치지 않는다",
           "#include <stdint.h>",
           "typedef struct { uint32_t t_ms; uint32_t id; uint8_t dlc; uint8_t d[8]; } vf_t;",
           "typedef struct { int after; uint32_t id; uint8_t d[8]; } vfc_t;",
           "typedef struct { const char *name; const vf_t *frames; int nframes; long long end_t_ms; const char *status;",
           "                 const uint8_t *payload; int npayload; int total; const vfc_t *fc; int nfc; } vcase_t;"]

    def hb(s):
        return [int(x, 16) for x in s.split()] if s.strip() else []

    for i, c in enumerate(doc["cases"]):
        fr = []
        for f in c["frames"]:
            d = hb(f["data"])
            assert len(d) <= 8, c["name"]
            fr.append("{%d,0x%s,%d,{%s}}" % (f["t_ms"], f["id"], len(d), ",".join("0x%02X" % x for x in d + [0] * (8 - len(d)))))
        out.append("static const vf_t k_vf%d[] = {%s};" % (i, ",\n  ".join(fr)))
        p = hb(c["expect"]["payload"])
        out.append("static const uint8_t k_vp%d[] = {%s};" % (i, ",".join("0x%02X" % x for x in p) or "0"))
        fcs = ["{%d,0x%s,{%s}}" % (x["after_frame"], x["id"], ",".join("0x%02X" % y for y in hb(x["data"]))) for x in c["expect"]["fc"]]
        out.append("static const vfc_t k_vc%d[] = {%s};" % (i, ",".join(fcs) or "{-1,0,{0}}"))
    out.append("static const vcase_t k_vcases[] = {")
    for i, c in enumerate(doc["cases"]):
        e = c["expect"]
        out.append('  {"%s", k_vf%d, %d, %d, "%s", k_vp%d, %d, %d, k_vc%d, %d},' % (
            c["name"], i, len(c["frames"]), c.get("end_t_ms", -1), e["status"], i, len(hb(e["payload"])), e["total"], i, len(e["fc"])))
    out.append("};")
    out.append("static const int k_nvcases = %d;" % len(doc["cases"]))
    with open(os.path.join(BUILD, "isotp_vectors.h"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    return len(doc["cases"])


def build_tests():
    n = gen_vectors_header()
    unit = sorted(glob.glob(os.path.join(HERE, "unit", "*.c")))
    ok = zig_cc(["-I" + BUILD, "-I" + os.path.join(HERE, "unit")] + CORE_SRC + SIM_SRC + unit + ["-o", os.path.join(BUILD, "tests.wasm")])
    print("build tests.wasm: %s (ISO-TP vectors %d)" % ("ok" if ok else "FAILED", n), flush=True)
    return ok


def build_sim():
    srcs = CORE_SRC + SIM_SRC + [os.path.join(ROOT, "port_sim", f) for f in ("sim_console_host.c", "sim_main.c")]
    ok = zig_cc(srcs + ["-o", os.path.join(BUILD, "sim.wasm")])
    print("build sim.wasm: %s" % ("ok" if ok else "FAILED"), flush=True)
    return ok


def run_tests(update):
    cmd = ["node", "--no-warnings", os.path.join(HERE, "run_tests.mjs"), os.path.join(BUILD, "tests.wasm"), os.path.join(HERE, "golden")]
    if update:
        cmd.append("--update")
    r = subprocess.run(cmd)
    return r.returncode == 0


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    update = "--update-golden" in sys.argv
    targets = args or ["tests", "sim"]
    os.makedirs(BUILD, exist_ok=True)
    if "sim" in targets and not build_sim():
        return 2
    if "tests" in targets:
        if not build_tests():
            return 2
        if not run_tests(update):
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
