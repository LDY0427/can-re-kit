// 명령 해석과 실행. v1 main.c의 명령을 그대로 옮기고(문구·줄 형식 포함) v2 줄과 안전 동작을 더했다.
// v1과 같은 문구는 PC 쪽 측정 스크립트가 찾는 것이라 바꾸지 않는다:
//   "송신 안 함", "ACK를 받았다", "# 멈춤", "# 프레임 N개, ID M종", "# 상태: 버스오류 N"
#include "sct/cli.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sct/busguard.h"
#include "sct/codes.h"
#include "sct/diag.h"
#include "sct/dtc.h"
#include "sct/out.h"
#include "sct/policy.h"
#include "sct/port.h"
#include "sct/stats.h"

#define REQ_TIMEOUT_MS 500       // v1 do_req
#define SCAN_TIMEOUT_MS 400      // v1 cmd_scan21
#define POLL_TIMEOUT_MS 300      // v1 cmd_poll
#define SCAN_GAP_MS 50           // v1: 훑기 번호 사이 50 ms
#define SCAN_CHECK_EVERY 8       // ELM327 프로그램(11-17 안전 수정)과 같다: 번호 8개마다 회전수·고장 코드 재확인
#define PIDS_GAP_MS 30           // v1 cmd_pids
#define POLL_REQS 8              // v1: 돌아가며 묻는 요청은 8가지까지
#define POLL_MIN_PERIOD_MS 5     // v1: 간격은 0(연속) 또는 5 ms 이상
#define POLL_MAX_COUNT 40000     // v1
#define POLL_NOACK_LIMIT 3       // v1: ACK 없음이 3번 이어지면 멈춘다
#define LISTEN_MAX_SEC 86400     // v2: 듣기 시간 상한(하루). v1은 상한이 없었다
#define CAPTURE_SLICE_MS 20      // v1 capture의 수신 대기
#define CAPTURE_CHECK_MS 100     // v2: 일반 모드에서 오류 상태(error passive)를 보는 간격
#define LOOPBACK_WAIT_MS 50      // [가정] 자기 수신 프레임은 송신 완료와 거의 같이 들어온다. 50 ms면 넉넉하다
#define LINGER_MS 30             // [가정] 명령 끝 ACK 꼬리 시간. ECU가 CF를 붙여 보내면(간격 0) 4,095바이트도 약 150 ms라 긴 응답은 다 못 덮는다
#define RSP_CAP 256              // v1 req·scan21의 응답 버퍼
#define RSP_CAP_SMALL 64         // v1 ping·pids·poll의 응답 버퍼

// 회전수를 읽는 요청(엔진 정지 확인과 poll의 회전수 칸에 쓴다). 차마다 다르다.
// 기본은 표준 01 0C. 표준 회전수가 없는 ECU는 rpmsrc 명령으로 바꾼다(예: KTM 390 Duke 2016은 rpmsrc 21 21).
// 값 = 응답의 서비스·번호 되돌림 바로 뒤 2바이트(큰 끝) ÷ div.
typedef struct { uint8_t req[7]; int n; double div; bool on; } rpm_src_t;
static rpm_src_t s_rpm = {{0x01, 0x0C}, 2, 4.0, true};

static int echo_len(const uint8_t *req)
{
    return req[0] == 0x22 ? 3 : 2; // 22는 번호가 2바이트
}

// rsp가 회전수 요청의 긍정 응답이면 회전수를 넣고 true
static bool rpm_of(const uint8_t *req, int n, const uint8_t *rsp, int len, double *rpm)
{
    if (!s_rpm.on || n != s_rpm.n || memcmp(req, s_rpm.req, (size_t)n) != 0) return false;
    int e = echo_len(req);
    if (len < e + 2 || rsp[0] != (uint8_t)(req[0] + 0x40) || memcmp(rsp + 1, req + 1, (size_t)(e - 1)) != 0) return false;
    *rpm = (rsp[e] * 256 + rsp[e + 1]) / s_rpm.div;
    return true;
}

static void rpm_src_text(char *buf, size_t cap)
{
    if (!s_rpm.on) {
        snprintf(buf, cap, "none");
        return;
    }
    size_t k = 0;
    for (int i = 0; i < s_rpm.n && k + 4 < cap; i++) k += (size_t)snprintf(buf + k, cap - k, i ? " %02X" : "%02X", s_rpm.req[i]);
    snprintf(buf + k, cap - k, " / %g", s_rpm.div);
}

// ---------------------------------------------------------------- 해석 도우미
static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool parse_hex(const char *t, int maxdig, uint32_t *out)
{
    size_t n = strlen(t);
    if (n < 1 || n > (size_t)maxdig) return false;
    uint32_t v = 0;
    for (size_t i = 0; i < n; i++) {
        int h = hexval((unsigned char)t[i]);
        if (h < 0) return false;
        v = v * 16 + (uint32_t)h;
    }
    *out = v;
    return true;
}

bool cli_parse_hex_byte(const char *t, uint8_t *out)
{
    uint32_t v;
    if (!parse_hex(t, 2, &v)) return false;
    *out = (uint8_t)v;
    return true;
}

bool cli_parse_hex_id(const char *t, uint32_t *out) { return parse_hex(t, 3, out); }

bool cli_parse_int(const char *t, int lo, int hi, int *out)
{
    size_t n = strlen(t);
    if (n < 1 || n > 9) return false; // 9자리면 int를 넘지 않는다
    long v = 0;
    for (size_t i = 0; i < n; i++) {
        if (t[i] < '0' || t[i] > '9') return false;
        v = v * 10 + (t[i] - '0');
    }
    if (v < lo || v > hi) return false;
    *out = (int)v;
    return true;
}

const char *sct_err_key(sct_err_t e)
{
    switch (e) {
    case SCT_E_NO_RESPONSE: return "no_response";
    case SCT_E_NO_ACK: return "no_ack";
    case SCT_E_DRIVER: return "driver_error";
    case SCT_E_BLOCKED: return "blocked_service";
    case SCT_E_TALK_OFF: return "talk_off";
    case SCT_E_BUS_ERR_REFUSED: return "bus_error_refused";
    case SCT_E_BUS_OFF: return "bus_off";
    case SCT_E_ERR_PASSIVE: return "error_passive";
    case SCT_E_JAM_STOP: return "jam_stop";
    case SCT_E_BAD_ARGS: return "bad_args";
    case SCT_E_BAD_HEX: return "bad_hex";
    case SCT_E_LINE_TOO_LONG: return "line_too_long";
    case SCT_E_BAD_SPEED: return "bad_speed";
    case SCT_E_BAD_TARGET: return "bad_target";
    case SCT_E_UNKNOWN_CMD: return "unknown_command";
    case SCT_E_ENGINE_RUNNING: return "engine_running";
    case SCT_E_RPM_UNKNOWN: return "rpm_unknown";
    case SCT_E_DTC_CHANGED: return "dtc_changed";
    case SCT_E_DTC_UNKNOWN: return "dtc_unknown";
    case SCT_E_LOOPBACK_REFUSED: return "loopback_refused";
    case SCT_E_LOOPBACK_FAIL: return "loopback_fail";
    case SCT_E_FORCE_REFUSED: return "force_refused";
    }
    return "unknown";
}

sct_err_t sct_err_of_status(int st)
{
    switch (st) {
    case SCT_NO_RESPONSE: return SCT_E_NO_RESPONSE;
    case SCT_NO_ACK: return SCT_E_NO_ACK;
    case SCT_DRIVER_ERR: return SCT_E_DRIVER;
    case SCT_BLOCKED: return SCT_E_BLOCKED;
    case SCT_TALK_OFF: return SCT_E_TALK_OFF;
    case SCT_BUS_ERR_REFUSED: return SCT_E_BUS_ERR_REFUSED;
    case SCT_BUS_OFF: return SCT_E_BUS_OFF;
    case SCT_ERR_PASSIVE: return SCT_E_ERR_PASSIVE;
    default: return (sct_err_t)0;
    }
}

static void err_line(sct_err_t e) { sct_outf("ERR,%d,%s\n", (int)e, sct_err_key(e)); }

static void now_text(char *buf, size_t cap) { sct_fmt_time(buf, cap, port_now_us()); }

const char *cli_build_date(void)
{
#ifdef SCT_BUILD_DATE
    return SCT_BUILD_DATE; // 빌드 스크립트가 정한 날짜(가상 빌드는 시험 결과가 날마다 바뀌지 않게 고정한다)
#else
    // __DATE__ = "Nov 18 2026" → "2026-11-18"
    static char buf[11];
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *d = __DATE__;
    int m = 0;
    for (int i = 0; i < 12; i++)
        if (!strncmp(d, months + 3 * i, 3)) m = i + 1;
    int day = (d[4] == ' ' ? 0 : d[4] - '0') * 10 + (d[5] - '0');
    snprintf(buf, sizeof buf, "%.4s-%02d-%02d", d + 7, m, day);
    return buf;
#endif
}

// ---------------------------------------------------------------- 공통 출력
static void print_id_table(void)
{
    int nid = stats_nids();
    sct_outf("# 프레임 %" PRIu32 "개, ID %d종\n", stats_frames(), nid);
    for (int i = 0; i < nid; i++) { // v1 형식 그대로
        const stats_id_t *r = stats_id_at(i);
        double per = r->n > 1 ? (double)(r->t_last - r->t_first) / 1000.0 / (double)(r->n - 1) : 0;
        sct_outf("ID,%" PRIX32 ",%" PRIu32 "개,평균주기 %.1f ms\n", r->id, r->n, per);
    }
    for (int i = 0; i < nid; i++) { // v2: ID 줄 뒤에 자세한 표
        const stats_id_t *r = stats_id_at(i);
        char chg[9], first[24], last[24], per[64];
        for (int b = 0; b < 8; b++) chg[b] = (r->changed >> b) & 1 ? '1' : '0';
        chg[8] = 0;
        int dlc = r->dlc > 8 ? 8 : r->dlc;
        sct_fmt_hex(first, sizeof first, r->first, (size_t)dlc);
        sct_fmt_hex(last, sizeof last, r->last, (size_t)dlc);
        if (r->n >= 3)
            snprintf(per, sizeof per, "%.2f,%.2f,%.2f,%.2f", r->per_mean_ms, r->per_min_ms, r->per_max_ms, stats_period_std_ms(r));
        else if (r->n == 2)
            snprintf(per, sizeof per, "%.2f,%.2f,%.2f,", r->per_mean_ms, r->per_min_ms, r->per_max_ms);
        else
            snprintf(per, sizeof per, ",,,");
        sct_outf("IDX,%s%" PRIX32 ",%" PRIu32 ",%d,%s,%s,%s,%s\n", r->ext ? "x" : "", r->id, r->n, r->dlc, per, chg, first, last);
    }
}

static void explain_fail(int r, const uint8_t *req)
{
    if (r == SCT_NO_ACK) sct_outf("# (드라이버를 내렸다. 다음 요청 때 다시 0.3초 듣고 들어간다)\n");
    if (r == SCT_TALK_OFF) sct_outf("# 송신 잠금: 보드의 DIP 2번(TALK)이 꺼져 있다. 요청을 보내려면 켠다(주황 LED). 듣기만 할 때는 꺼 둔다\n");
    else if (r == SCT_NO_ACK) sct_outf("# ACK 없음: 받아 주는 장치가 없다. 속도, H·L 결선, 종단저항, ECU 전원(키·킬 스위치)을 확인\n");
    else if (r == SCT_DRIVER_ERR) sct_outf("# 드라이버 오류. 드라이버를 내렸다\n");
    else if (r == SCT_BUS_ERR_REFUSED) sct_outf("# 요청을 보내지 않았다. listen 으로 속도를 먼저 확인한다\n");
    else if (r == SCT_BUS_OFF) sct_outf("# BUS-OFF: 송신 오류가 쌓여 버스에서 떨어졌다. 드라이버를 내렸다. 속도와 결선을 다시 확인\n");
    else if (r == SCT_ERR_PASSIVE) sct_outf("# 오류 수동 상태: 오류 카운터가 128을 넘어 드라이버를 내렸다. 속도와 결선, 종단저항을 다시 확인\n");
    else if (r == SCT_BLOCKED) {
        if (req) sct_outf("# 막은 요청: 서비스 %02X(%s)는 보내지 않는다. 읽기 서비스(%s)만 보낸다\n", req[0], policy_service_name(req[0]), SCT_ALLOWED_TEXT);
        else sct_outf("# 막은 요청: 읽기 서비스(%s)만 보낸다\n", SCT_ALLOWED_TEXT);
    } else sct_outf("# 응답 없음(ACK는 받음): 속도와 결선은 맞다. 이 요청을 모르거나 주소가 다르다\n");
    sct_err_t e = sct_err_of_status(r);
    if (e) err_line(e);
}

static void print_answer(const uint8_t *req, int n, const uint8_t *rsp, int len, uint32_t rsp_id, int64_t lat)
{
    char tm[32], pre[64];
    now_text(tm, sizeof tm);
    snprintf(pre, sizeof pre, "ANS,%s,%" PRIX32 ",%.2f ms,", tm, rsp_id, (double)lat / 1000.0);
    sct_out_hex(pre, rsp, (size_t)len, "\n");
    if (len >= 3 && rsp[0] == 0x7F) {
        sct_outf("# 거부 응답: 서비스 %02X, 코드 %02X (%s)\n", rsp[1], rsp[2], policy_nrc_name(rsp[2]));
        return;
    }
    double rpm;
    if (rpm_of(req, n, rsp, len, &rpm) && !(req[0] == 0x01 && req[1] == 0x0C)) {
        char src[40];
        rpm_src_text(src, sizeof src);
        sct_outf("# 회전수 %.1f rpm (rpmsrc %s)\n", rpm, src);
    }
    if (n == 2 && req[0] == 0x01 && len >= 3 && rsp[0] == 0x41) {
        uint8_t a = rsp[2], b = len >= 4 ? rsp[3] : 0;
        switch (req[1]) { // [표준] SAE J1979 식
        case 0x04: sct_outf("# 부하 %.1f %%\n", a * 100 / 255.0); break;
        case 0x05: sct_outf("# 수온 %d C\n", a - 40); break;
        case 0x0B: sct_outf("# 흡기압 %d kPa\n", a); break;
        case 0x0C: sct_outf("# 회전수 %.1f rpm\n", (a * 256 + b) / 4.0); break;
        case 0x0D: sct_outf("# 속도 %d km/h\n", a); break;
        case 0x0E: sct_outf("# 점화시기 %.1f deg\n", a / 2.0 - 64); break;
        case 0x0F: sct_outf("# 흡기온 %d C\n", a - 40); break;
        case 0x11: sct_outf("# 스로틀 %.1f %%\n", a * 100 / 255.0); break;
        case 0x42: sct_outf("# ECU 전압 %.3f V\n", (a * 256 + b) / 1000.0); break;
        default: break;
        }
    }
}

// 진단 요청 하나와 그 출력(v1 do_req). verbose면 REQ 줄과 실패 설명을 낸다
// explain_noresp = false면 "응답 없음"을 설명하지 않는다(부르는 쪽이 따로 알린다)
static void do_req_ex(const uint8_t *req, int n, uint8_t *rsp, int cap, bool verbose, bool explain_noresp, diag_result_t *res)
{
    if (n < 1 || n > 7 || !policy_service_allowed(req[0])) {
        // v1은 막은 요청에도 REQ 줄을 먼저 찍었다. REQ 줄이 "보냈다"로 읽히지 않게 v2는 찍지 않는다
        memset(res, 0, sizeof *res);
        res->status = SCT_BLOCKED;
        res->txdone_us = -1;
        if (verbose) explain_fail(SCT_BLOCKED, req);
        return;
    }
    if (verbose) {
        char tm[32], pre[64], suf[24];
        now_text(tm, sizeof tm);
        snprintf(pre, sizeof pre, "REQ,%s,%" PRIX32 ",%d,", tm, diag_target(), n);
        snprintf(suf, sizeof suf, ",%dkbps\n", diag_speed());
        sct_out_hex(pre, req, (size_t)n, suf);
    }
    diag_request(req, n, rsp, cap, REQ_TIMEOUT_MS, res);
    if (res->status == SCT_OK && res->partial) {
        sct_outf("PARTIAL,%d/%d\n", res->len, res->total);
        if (res->isotp_err != ISOTP_ERR_NONE) sct_outf("ISOTPERR,%s,%d/%d\n", isotp_err_key(res->isotp_err), res->len, res->total);
    }
    if (res->status == SCT_OK) print_answer(req, n, rsp, res->len, res->rsp_id, res->lat_us);
    else if (verbose && (explain_noresp || res->status != SCT_NO_RESPONSE)) explain_fail(res->status, req);
}

static void do_req(const uint8_t *req, int n, uint8_t *rsp, int cap, bool verbose, diag_result_t *res)
{
    do_req_ex(req, n, rsp, cap, verbose, true, res);
}

// ---------------------------------------------------------------- 듣기
typedef struct {
    int frames;     // -1 = 시작하지 못함
    double sec;
    int stop;       // 0 = 끝까지, SCT_E_JAM_STOP, SCT_ERR_PASSIVE 계열 상태
} capture_t;

// 듣기(또는 일반 모드로 지켜보기). v1 capture + 오류 수동 상태 감시
static capture_t capture(int kbps, int sec, bool normal, bool quiet)
{
    capture_t c = {.frames = -1};
    stats_reset();
    if (normal ? busguard_enter_normal(kbps) != 0 : !busguard_open(kbps, SCT_MODE_LISTEN)) return c;
    int64_t t_start = port_now_us();
    int64_t t_end = t_start + (int64_t)sec * 1000000, t_tick = t_start + 1000000, t_check = t_start + CAPTURE_CHECK_MS * 1000;
    uint32_t e_last = 0, f_last = 0;
    sct_frame_t f;
    while (port_now_us() < t_end && !port_con_key_pressed()) {
        if (port_can_recv(&f, CAPTURE_SLICE_MS)) {
            stats_add(&f);
            port_led(SCT_LED_RX, stats_frames() & 1); // 프레임마다 깜박인다
            if (!quiet && sct_out_print_enabled()) sct_out_frame("RX", &f);
        }
        int64_t now = port_now_us();
        if (normal && now >= t_check) {
            t_check = now + CAPTURE_CHECK_MS * 1000;
            int chk = busguard_check();
            if (chk) {
                c.stop = chk;
                err_line(sct_err_of_status(chk));
                break;
            }
        }
        if (now >= t_tick) { // 1초마다: 프레임과 버스 오류가 얼마나 늘었는지
            t_tick += 1000000;
            uint32_t e = busguard_bus_errors();
            if (!quiet) sct_outf("SEC,%" PRId64 ",프레임 +%" PRIu32 ",버스오류 +%" PRIu32 "\n", now / 1000000, stats_frames() - f_last, e - e_last);
            if (busguard_jam_verdict(normal, stats_frames() - f_last, e - e_last)) {
                sct_outf("# 멈춤: 일반 모드에서 1초 동안 프레임 0개, 버스 오류 %" PRIu32 "건. 속도나 H·L 결선을 다시 본다\n", e - e_last);
                err_line(SCT_E_JAM_STOP);
                c.stop = SCT_E_JAM_STOP;
                break;
            }
            e_last = e;
            f_last = stats_frames();
        }
    }
    c.frames = (int)stats_frames();
    c.sec = (double)(port_now_us() - t_start) / 1e6;
    return c;
}

static void cmd_listen(int kbps, int sec, bool normal)
{
    sct_outf("# %s %d kbps, %d초. 아무 키나 누르면 멈춤\n", normal ? "일반 모드(ACK 줌)" : "듣기 전용", kbps, sec);
    if (normal && !port_talk_enabled()) sct_outf("# TALK 스위치가 꺼져 있어 ACK가 버스로 나가지 않는다(듣기 전용과 같다)\n");
    capture_t c = capture(kbps, sec, normal, false);
    if (c.frames < 0) return;
    print_id_table();
    uint64_t bits = stats_bits();
    sct_outf("BUSLOAD,%d,%.2f,%d,%" PRIu64 ",%.2f\n", kbps, c.sec, c.frames, bits, stats_bus_load_pct(bits, kbps, c.sec));
    busguard_print_status();
    busguard_close();
}

static void cmd_auto(void)
{
    static const int rates[] = {500, 250, 125, 1000};
    sct_outf("# 듣기 전용으로 속도 4가지를 5초씩 본다\n");
    for (int i = 0; i < 4; i++) {
        capture_t c = capture(rates[i], 5, false, true);
        if (c.frames < 0) continue;
        int n = c.frames;
        uint32_t e = busguard_bus_errors();
        const char *v = (n && !e)   ? "이 속도가 맞다"
                        : (n && e)  ? "일부만 읽힘(잡음 또는 속도 근접)"
                        : (!n && e) ? "신호는 있는데 이 속도가 아니다"
                                    : "신호 없음(조용한 버스이거나 결선·전원 문제)";
        sct_outf("AUTO,%d kbps,프레임 %d,ID %d종,버스오류 %" PRIu32 ",%s\n", rates[i], n, stats_nids(), e, v);
        busguard_close();
    }
    sct_outf("# 네 속도 모두 '신호 없음'이면 ECU가 먼저 말하지 않는 것일 수 있다. 다음은 ping 500\n");
    sct_outf("# 요청 속도는 그대로 %d kbps다(auto와 listen은 요청 속도를 바꾸지 않는다)\n", diag_speed());
}

// ---------------------------------------------------------------- 요청
static void cmd_ping(int kbps)
{
    if (!port_talk_enabled()) {
        explain_fail(SCT_TALK_OFF, NULL);
        return;
    }
    if (!policy_speed_ok(kbps)) {
        sct_outf("# 지원 속도: 1000 500 250 125 100\n");
        err_line(SCT_E_BAD_SPEED);
        return;
    }
    sct_outf("# %d kbps: 0.3초 듣고, 오류가 없으면 요청 한 번(재전송 없음): 01 00 (지원 항목 묻기)\n", kbps);
    uint8_t req[2] = {0x01, 0x00}, rsp[RSP_CAP_SMALL];
    int old = diag_speed();
    diag_set_speed(kbps);
    diag_result_t res;
    do_req(req, 2, rsp, sizeof rsp, true, &res);
    if (res.status >= 0) sct_outf("# ACK를 받았다 = 속도 %d kbps와 결선이 맞다. 요청 속도를 %d kbps로 둔다\n", kbps, kbps);
    else diag_set_speed(old);
    busguard_print_status();
}

static void cmd_pids(void)
{
    uint8_t rsp[RSP_CAP_SMALL], sup[0x81] = {0};
    diag_result_t res;
    for (int base = 0; base < 0x80; base += 0x20) {
        uint8_t req[2] = {0x01, (uint8_t)base};
        do_req(req, 2, rsp, sizeof rsp, true, &res);
        if (res.status != SCT_OK || res.len < 6 || rsp[0] != 0x41) break;
        for (int i = 0; i < 32; i++)
            if (rsp[2 + i / 8] & (0x80 >> (i % 8))) sup[base + i + 1] = 1; // base + 32 = 다음 묶음이 있다는 표시
        if (!(rsp[5] & 1)) break;                                        // 다음 묶음 없음
    }
    for (int p = 1; p < 0x80; p++) {
        if (!sup[p] || p % 0x20 == 0) continue;
        uint8_t req[2] = {0x01, (uint8_t)p};
        do_req(req, 2, rsp, sizeof rsp, true, &res);
        if (res.status < 0) break; // 송신이 안 되면 멈춘다
        port_sleep_ms(PIDS_GAP_MS);
        if (port_con_key_pressed()) break;
    }
}

// ---------------------------------------------------------------- 고장 코드
typedef struct {
    int st03, st07;    // diag 상태. 응답이 긍정이 아니면 내용 비교에서 그 상태로 본다
    uint16_t n03, n07;
    uint8_t d03[RSP_CAP], d07[RSP_CAP];
} dtc_snap_t;

// 조용히 읽는다(REQ·ANS 줄 없음). 송신이 실패하면 그 상태를 돌려준다(0 이상이면 송신은 됐다)
static int quiet_req(const uint8_t *req, int n, uint8_t *rsp, int cap, diag_result_t *res)
{
    diag_request(req, n, rsp, cap, SCAN_TIMEOUT_MS, res);
    return res->status;
}

static int snap_dtc(dtc_snap_t *d)
{
    static const uint8_t r03[1] = {0x03}, r07[1] = {0x07};
    diag_result_t res;
    memset(d, 0, sizeof *d);
    d->st03 = quiet_req(r03, 1, d->d03, sizeof d->d03, &res);
    if (d->st03 < 0) return d->st03;
    d->n03 = res.len;
    if (res.partial) d->st03 = SCT_NO_RESPONSE; // 깨진 응답은 기준으로 쓰지 않는다
    d->st07 = quiet_req(r07, 1, d->d07, sizeof d->d07, &res);
    if (d->st07 < 0) return d->st07;
    d->n07 = res.len;
    if (res.partial) d->st07 = SCT_NO_RESPONSE;
    return SCT_OK;
}

static bool snap_ok03(const dtc_snap_t *d) { return d->st03 == SCT_OK && d->n03 >= 1 && d->d03[0] == 0x43; }

static bool snap_equal(const dtc_snap_t *a, const dtc_snap_t *b)
{
    return a->st03 == b->st03 && a->st07 == b->st07 && a->n03 == b->n03 && a->n07 == b->n07 && !memcmp(a->d03, b->d03, a->n03) &&
           !memcmp(a->d07, b->d07, a->n07);
}

static void snap_codes_text(const dtc_snap_t *d, char *buf, size_t cap)
{
    dtc_list_t l3, l7;
    char c[6];
    size_t w = 0;
    buf[0] = 0;
    bool ok3 = d->st03 == SCT_OK && dtc_decode(0x03, d->d03, d->n03, &l3);
    bool ok7 = d->st07 == SCT_OK && dtc_decode(0x07, d->d07, d->n07, &l7);
    w += (size_t)snprintf(buf + w, cap - w, "03:");
    for (int i = 0; ok3 && i < l3.n && w + 8 < cap; i++) {
        dtc_text(l3.codes[i], c);
        w += (size_t)snprintf(buf + w, cap - w, " %s", c);
    }
    if (!ok3 && w + 8 < cap) w += (size_t)snprintf(buf + w, cap - w, " 못 읽음");
    if (w + 8 < cap) w += (size_t)snprintf(buf + w, cap - w, " / 07:");
    for (int i = 0; ok7 && i < l7.n && w + 8 < cap; i++) {
        dtc_text(l7.codes[i], c);
        w += (size_t)snprintf(buf + w, cap - w, " %s", c);
    }
    if (!ok7 && w + 10 < cap) snprintf(buf + w, cap - w, " 못 읽음");
}

static void cmd_dtc(void)
{
    static const uint8_t svcs[] = {0x03, 0x07, 0x0A};
    uint8_t rsp[RSP_CAP];
    diag_result_t res;
    sct_outf("# 고장 코드 읽기: 03(확정), 07(미확정), 0A(영구) 차례로. 지우는 요청(04, 14)은 보낼 방법이 없다\n");
    for (size_t i = 0; i < sizeof svcs; i++) {
        uint8_t req[1] = {svcs[i]};
        bool is_0a = svcs[i] == 0x0A;
        do_req_ex(req, 1, rsp, sizeof rsp, true, !is_0a, &res);
        if (res.status < 0) break; // 송신이 안 되면 멈춘다(설명은 do_req가 냈다)
        if (res.status == SCT_NO_RESPONSE && is_0a) {
            sct_outf("# 0A: 응답 없음(이 ECU는 0A에 답하지 않았다 [실측 11-17]). 오류가 아니다\n");
            continue;
        }
        if (res.status != SCT_OK) continue;
        dtc_list_t l;
        if (!dtc_decode(svcs[i], rsp, res.len, &l)) continue; // 거부 응답
        char codes[DTC_MAX * 6 + 1];
        size_t w = 0;
        codes[0] = 0;
        for (int k = 0; k < l.n; k++) {
            char c[6];
            dtc_text(l.codes[k], c);
            w += (size_t)snprintf(codes + w, sizeof codes - w, k ? " %s" : "%s", c);
        }
        char pre[DTC_MAX * 6 + 32];
        snprintf(pre, sizeof pre, "DTC,%02X,%d,%s,", svcs[i], l.n, codes);
        sct_out_hex(pre, rsp, res.len, "\n");
        if (l.count_mismatch)
            sct_outf("# 개수 바이트 %02X와 실린 쌍 수 %d가 다르다. 개수와 실린 바이트 중 적은 쪽까지 읽고 0000은 건너뛰었다\n", l.count_byte & 0xFF,
                     (res.len - 2) / 2);
    }
}

// ---------------------------------------------------------------- scan21
typedef enum { RPM_OK, RPM_UNKNOWN, RPM_TX_FAIL } rpm_read_t;

static rpm_read_t read_rpm(double *rpm, int *st)
{
    if (!s_rpm.on) { // rpmsrc none: 엔진 정지는 사람이 확인한다
        *rpm = 0;
        *st = SCT_OK;
        return RPM_OK;
    }
    uint8_t rsp[RSP_CAP_SMALL];
    diag_result_t res;
    *st = quiet_req(s_rpm.req, s_rpm.n, rsp, sizeof rsp, &res);
    if (*st < 0) return RPM_TX_FAIL;
    if (*st != SCT_OK || res.partial || !rpm_of(s_rpm.req, s_rpm.n, rsp, res.len, rpm)) return RPM_UNKNOWN;
    return RPM_OK;
}

// 훑는 중 점검. 0 = 계속, 아니면 ERR 번호(멈춤 문구는 여기서 찍는다)
static sct_err_t scan_guard(const dtc_snap_t *base, int lid)
{
    double rpm = 0;
    int st = 0;
    rpm_read_t r = read_rpm(&rpm, &st);
    if (r == RPM_TX_FAIL) {
        explain_fail(st, NULL);
        return sct_err_of_status(st);
    }
    if (r != RPM_OK || rpm > 0) {
        if (r != RPM_OK) sct_outf("# 멈춤: 회전수를 읽지 못했다. 21 %02X 앞에서 멈췄다\n", lid);
        else sct_outf("# 멈춤: 회전수 %.0f rpm, 엔진이 돌고 있다. 21 %02X 앞에서 멈췄다\n", rpm, lid);
        sct_outf("# 고장 코드는 지우지 말고 그대로 둔 채 알린다\n");
        return r != RPM_OK ? SCT_E_RPM_UNKNOWN : SCT_E_ENGINE_RUNNING;
    }
    dtc_snap_t now;
    st = snap_dtc(&now);
    if (st < 0) {
        explain_fail(st, NULL);
        return sct_err_of_status(st);
    }
    if (!snap_equal(base, &now)) {
        char a[200], b[200];
        snap_codes_text(base, a, sizeof a);
        snap_codes_text(&now, b, sizeof b);
        sct_outf("# 멈춤: 고장 코드가 달라졌다. 21 %02X 앞에서 멈췄다(직전 %d개 안에서 생겼다)\n", lid, SCAN_CHECK_EVERY);
        sct_outf("#   전 %s\n", a);
        sct_outf("#   지금 %s\n", b);
        sct_outf("# 고장 코드는 지우지 말고 그대로 둔 채 알린다\n");
        return SCT_E_DTC_CHANGED;
    }
    sct_outf("# 점검(21 %02X 앞): 회전수 0 rpm, 고장 코드 같음\n", lid);
    return (sct_err_t)0;
}

// 서비스 0x21의 번호를 차례로 읽는다(읽기만). 엔진 정지를 ECU 회전수로 확인한 뒤에만 훑는다.
static void cmd_scan21(int from, int to)
{
    uint8_t rsp[RSP_CAP];
    sct_outf("# 서비스 21, 번호 %02X~%02X 읽기. 아무 키나 누르면 멈춤\n", from, to);
    double rpm = 0;
    int st = 0;
    rpm_read_t r = read_rpm(&rpm, &st);
    if (r == RPM_TX_FAIL) {
        explain_fail(st, NULL);
        return;
    }
    if (r != RPM_OK) {
        char src[40];
        rpm_src_text(src, sizeof src);
        sct_outf("# 훑지 않음: 회전수(rpmsrc %s)를 읽지 못해 엔진 정지를 확인할 수 없다. 이 ECU의 회전수 요청을 rpmsrc 로 정한다\n", src);
        err_line(SCT_E_RPM_UNKNOWN);
        return;
    }
    if (rpm > 0) {
        sct_outf("# 훑지 않음: ECU 회전수 %.0f rpm. 엔진이 돌고 있다. 엔진을 끄고 다시 한다(엔진이 도는 중에는 정해진 번호만 req 로 읽는다)\n", rpm);
        err_line(SCT_E_ENGINE_RUNNING);
        return;
    }
    dtc_snap_t base;
    st = snap_dtc(&base);
    if (st < 0) {
        explain_fail(st, NULL);
        return;
    }
    if (!snap_ok03(&base)) {
        sct_outf("# 훑지 않음: 고장 코드(03)를 읽지 못해 비교 기준이 없다\n");
        err_line(SCT_E_DTC_UNKNOWN);
        return;
    }
    char codes[200];
    snap_codes_text(&base, codes, sizeof codes);
    if (!s_rpm.on) sct_outf("# rpmsrc none: 엔진 정지를 ECU로 확인하지 않는다. 키 ON·엔진 정지는 사람이 확인한다\n");
    sct_outf("# 엔진 정지 확인: ECU 회전수 0 rpm. 번호 %d개마다 회전수와 고장 코드를 다시 읽는다\n", SCAN_CHECK_EVERY);
    sct_outf("#   고장 코드 기준 %s\n", codes);

    int noack = 0;
    bool stopped = false, completed = false;
    for (int lid = from, i = 0; lid <= to; lid++, i++) {
        if (i && i % SCAN_CHECK_EVERY == 0) {
            sct_err_t g = scan_guard(&base, lid);
            if (g) {
                err_line(g);
                stopped = true;
                break;
            }
        }
        uint8_t req[2] = {0x21, (uint8_t)lid};
        diag_result_t res;
        int rr = quiet_req(req, 2, rsp, sizeof rsp, &res);
        if (rr == SCT_OK && rsp[0] == 0x7F) {
            uint8_t code = res.len >= 3 ? rsp[2] : 0;
            sct_outf("LID,%02X,거부,%02X,%s\n", lid, code, policy_nrc_name(code));
        } else if (rr == SCT_OK) {
            char pre[48];
            if (res.partial) snprintf(pre, sizeof pre, "LID,%02X,PARTIAL %d/%d,", lid, res.len, res.total);
            else snprintf(pre, sizeof pre, "LID,%02X,%d,", lid, res.len);
            sct_out_hex(pre, rsp, res.len, "\n");
            if (res.partial && res.isotp_err != ISOTP_ERR_NONE)
                sct_outf("ISOTPERR,%s,%d/%d\n", isotp_err_key(res.isotp_err), res.len, res.total);
        } else if (rr == SCT_NO_RESPONSE)
            sct_outf("LID,%02X,응답없음\n", lid);
        else if (rr == SCT_NO_ACK) {
            sct_outf("LID,%02X,ACK없음\n", lid);
            if (++noack >= POLL_NOACK_LIMIT) {
                sct_outf("# ACK 없음이 3번 이어져 멈춘다\n");
                err_line(SCT_E_NO_ACK);
                stopped = true;
                break;
            }
        } else {
            sct_outf("LID,%02X,송신실패,%d\n", lid, rr);
            explain_fail(rr, req);
            stopped = true;
            break;
        }
        if (rr >= 0) noack = 0;
        port_sleep_ms(SCAN_GAP_MS);
        if (port_con_key_pressed()) break;
        if (lid == to) completed = true;
    }
    if (completed && !stopped) { // 끝까지 훑었으면 고장 코드를 한 번 더 비교한다
        dtc_snap_t after;
        st = snap_dtc(&after);
        if (st < 0) explain_fail(st, NULL);
        else if (snap_equal(&base, &after)) sct_outf("# 고장 코드: 훑기 전과 같다\n");
        else {
            char a[200], b[200];
            snap_codes_text(&base, a, sizeof a);
            snap_codes_text(&after, b, sizeof b);
            sct_outf("# 멈춤: 고장 코드가 훑기 전과 달라졌다\n");
            sct_outf("#   전 %s\n", a);
            sct_outf("#   지금 %s\n", b);
            sct_outf("# 고장 코드는 지우지 말고 그대로 둔 채 알린다\n");
            err_line(SCT_E_DTC_CHANGED);
        }
    }
    busguard_print_status();
}

// ---------------------------------------------------------------- poll
// 같은 요청을 일정 간격으로 반복한다(v1 cmd_poll). 응답 지연과 초당 횟수, 송신 완료 시간을 잰다.
static void cmd_poll(int period_ms, int count, uint8_t reqs[][7], const int *lens, int nreq, int every)
{
    static stats_lat_t lat; // 표본 2,000개와 10 us 칸 2,001개(약 12 kB)라 스택에 두지 않는다
    int q_ok[POLL_REQS] = {0}, q_fail[POLL_REQS] = {0};
    int64_t q_sum[POLL_REQS] = {0};
    uint8_t rsp[RSP_CAP_SMALL];
    int ok = 0, fail = 0, noack = 0, late = 0;
    uint32_t tx_n = 0;
    int64_t tx_sum = 0, tx_min = 0, tx_max = 0;
    stats_lat_reset(&lat);
    diag_pending_reset();
    if (period_ms) sct_outf("# %d ms 간격으로 %d번, 요청 %d가지. 아무 키나 누르면 멈춤\n", period_ms, count, nreq);
    else sct_outf("# 응답을 받자마자 다음 요청(사이 1 ms), %d번, 요청 %d가지. 아무 키나 누르면 멈춤\n", count, nreq);
    // 시간을 재기 전에 일반 모드로 들어간다(0.3초 듣기 포함). v1·v2.0.0은 시작 시각을 먼저 잡아, 요약의 시간과 초당 횟수에
    // 이 0.3초가 들어갔고 첫 요청 몇 개가 밀린 일정을 따라잡느라 붙어서 나갔다.
    // [실측 11-18] 10 ms 간격 300번이 3.31초·초당 90.6으로 찍혔다(실제는 약 3.01초·초당 약 100).
    int pre = port_talk_enabled() ? busguard_ensure_normal(diag_speed()) : 0; // TALK이 꺼져 있으면 첫 요청이 알린다
    int64_t t_start = port_now_us();
    for (int i = 0; i < count; i++) {
        int64_t t_next = t_start + (int64_t)(i + 1) * period_ms * 1000;
        int qi = (every > 0 && nreq > 1) ? ((i % every) == every - 1 ? 1 + (i / every) % (nreq - 1) : 0) : i % nreq;
        int n = lens[qi];
        const uint8_t *req = reqs[qi];
        diag_result_t res;
        if (i == 0 && pre) { // 들어가지 못했다. 이유는 위에서 이미 찍혔다. 다시 듣지 않고 실패 한 번으로 적는다
            memset(&res, 0, sizeof res);
            res.status = pre;
            res.txdone_us = -1;
            busguard_close();
        } else diag_request(req, n, rsp, sizeof rsp, POLL_TIMEOUT_MS, &res);
        int r = res.status;
        if (res.txdone_us >= 0) {
            if (!tx_n || res.txdone_us < tx_min) tx_min = res.txdone_us;
            if (!tx_n || res.txdone_us > tx_max) tx_max = res.txdone_us;
            tx_n++;
            tx_sum += res.txdone_us;
        }
        bool continue_ok = r == SCT_NO_RESPONSE; // 응답 없음(ACK는 받음)은 계속 본다
        if (r == SCT_NO_RESPONSE) noack = 0;
        char tm[32];
        if (r == SCT_OK) {
            ok++;
            noack = 0;
            stats_lat_add(&lat, res.lat_us);
            if (period_ms && res.lat_us > (int64_t)period_ms * 1000) late++; // 응답이 요청 간격보다 늦게 왔다
            q_ok[qi]++;
            q_sum[qi] += res.lat_us;
            char pre[64], suf[24] = "\n";
            now_text(tm, sizeof tm);
            snprintf(pre, sizeof pre, "POLL,%s,%.2f,", tm, (double)res.lat_us / 1000.0);
            double prpm;
            if (rpm_of(req, n, rsp, res.len, &prpm)) snprintf(suf, sizeof suf, ",%.1f\n", prpm);
            sct_out_hex(pre, rsp, res.len, suf);
        } else {
            fail++;
            q_fail[qi]++;
            now_text(tm, sizeof tm);
            sct_outf("POLL,%s,실패,%d\n", tm, r);
            if (r == SCT_NO_ACK && ++noack < POLL_NOACK_LIMIT) continue_ok = true;
            if (r < 0 && !continue_ok) {
                explain_fail(r, req);
                break;
            }
        }
        if (port_con_key_pressed()) break;
        if (!period_ms) {
            port_sleep_ms(1);
            continue;
        }
        int64_t wait = t_next - port_now_us();
        if (wait > 0) port_sleep_ms((int)(wait / 1000));
    }
    double sec = (double)(port_now_us() - t_start) / 1e6;
    if (ok)
        sct_outf("# 성공 %d, 실패 %d, %.1f초, 초당 %.1f번, 응답 지연 평균 %.2f ms (최소 %.2f, 최대 %.2f)\n", ok, fail, sec, sec > 0 ? ok / sec : 0,
                 (double)lat.sum / 1000.0 / ok, (double)lat.min / 1000.0, (double)lat.max / 1000.0);
    else sct_outf("# 성공 %d, 실패 %d, %.1f초, 초당 %.1f번\n", ok, fail, sec, 0.0);
    int64_t med = 0, p95 = 0;
    if (ok && stats_lat_quantiles(&lat, &med, &p95)) // PC 프로그램이 읽는 요약 줄(v1 형식)
        sct_outf("POLLSUM,%d,%d,%d,%d,%.2f,%.1f,%.2f,%.2f,%.2f,%.2f,%.2f,%d\n", period_ms, ok + fail, ok, fail, sec, sec > 0 ? ok / sec : 0,
                 (double)lat.min / 1000.0, (double)med / 1000.0, (double)lat.sum / 1000.0 / ok, (double)p95 / 1000.0, (double)lat.max / 1000.0, late);
    else sct_outf("POLLSUM,%d,%d,0,%d,%.2f,0,,,,,,0\n", period_ms, fail, fail, sec);
    // 지연 분포: 2 ms 미만, 2~5, 5~10, 10~20, 20~50, 50~100, 100 ms 이상. 끝은 응답 대기(7F xx 78)를 받은 횟수
    sct_outf("POLLHIST,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",대기응답 %d\n", lat.hist[0], lat.hist[1], lat.hist[2],
             lat.hist[3], lat.hist[4], lat.hist[5], lat.hist[6], diag_pending_total());
    for (int q = 0; q < nreq; q++) { // 요청별: 순번,요청,성공,실패,평균 지연(ms),초당
        char pre[24], suf[64];
        snprintf(pre, sizeof pre, "POLLREQ,%d,", q);
        snprintf(suf, sizeof suf, ",%d,%d,%.2f,%.1f\n", q_ok[q], q_fail[q], q_ok[q] ? (double)q_sum[q] / 1000.0 / q_ok[q] : 0, sec > 0 ? q_ok[q] / sec : 0);
        sct_out_hex(pre, reqs[q], (size_t)lens[q], suf);
    }
    if (tx_n)
        sct_outf("POLLTX,%" PRIu32 ",%.3f,%.3f,%.3f\n", tx_n, (double)tx_sum / 1000.0 / tx_n, (double)tx_min / 1000.0, (double)tx_max / 1000.0);
    else sct_outf("POLLTX,0,,,\n");
    busguard_print_status();
}

// ---------------------------------------------------------------- loopback bench
static void cmd_loopback(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "bench") != 0) {
        sct_outf("# 책상 전용 자체 시험이다. 차에 물리지 않은 것을 확인하고 loopback bench 로 부른다\n");
        err_line(SCT_E_BAD_ARGS);
        return;
    }
    if (!port_talk_enabled()) {
        explain_fail(SCT_TALK_OFF, NULL);
        return;
    }
    int kbps = diag_speed();
    sct_outf("# 책상 자체 시험(%d kbps): 0.3초 듣고, 조용하면 자기 수신 모드로 7DF 02 01 00 을 한 번 보낸다\n", kbps);
    if (!busguard_open(kbps, SCT_MODE_LISTEN)) return;
    sct_frame_t f;
    uint32_t n = 0;
    int64_t t_end = port_now_us() + (int64_t)BG_PRE_LISTEN_MS * 1000;
    while (port_now_us() < t_end) {
        if (!port_can_recv(&f, 10)) continue;
        n++;
        if (sct_out_print_enabled()) sct_out_frame("RX", &f);
    }
    uint32_t e = busguard_bus_errors();
    if (n || e) {
        sct_outf("# 거절: 버스가 조용하지 않다(프레임 %" PRIu32 "개, 버스 오류 %" PRIu32 "건). 차나 다른 장치에 물려 있으면 하지 않는다\n", n, e);
        err_line(SCT_E_LOOPBACK_REFUSED);
        busguard_close();
        return;
    }
    if (!busguard_open(kbps, SCT_MODE_SELFTEST)) return;
    static const uint8_t probe[2] = {0x01, 0x00};
    sct_frame_t tx = {.id = SCT_REQ_ID_FUNCTIONAL, .dlc = 8};
    isotp_make_sf(probe, 2, tx.data);
    int64_t t0 = port_now_us(), t_done = 0;
    sct_tx_result_t r = port_can_send_once(&tx, DIAG_TX_WAIT_MS, &t_done);
    if (r != SCT_TX_ACKED) {
        sct_outf("# 송신 실패(%d): 송신 완료 알림이 없다. 트랜시버 전원과 TX 핀 결선을 본다\n", (int)r);
        err_line(SCT_E_LOOPBACK_FAIL);
        busguard_print_status();
        busguard_close();
        return;
    }
    bool got = false;
    t_end = port_now_us() + (int64_t)LOOPBACK_WAIT_MS * 1000;
    while (port_now_us() < t_end) {
        if (!port_can_recv(&f, 5)) continue;
        sct_out_frame("RX", &f);
        if (!f.ext && f.id == tx.id && f.dlc == tx.dlc && !memcmp(f.data, tx.data, 8)) {
            got = true;
            break;
        }
    }
    if (got) sct_outf("# 되받음: 송신 → 트랜시버 → 수신 경로가 돈다(송신 완료 %.2f ms)\n", (double)(t_done - t0) / 1000.0);
    else {
        sct_outf("# 되받지 못했다: 수신 핀 결선이나 트랜시버의 수신 출력을 본다\n");
        err_line(SCT_E_LOOPBACK_FAIL);
    }
    busguard_print_status();
    busguard_close();
}

// ---------------------------------------------------------------- 명령 줄
static const char *const k_help[] = {
    "#   auto                  듣기 전용으로 500/250/125/1000을 5초씩",
    "#   listen <kbps> [초=30]  듣기 전용으로 받아 적기 (ACK를 주지 않는다)",
    "#   watch <kbps> [초=30]   일반 모드로 받아 적기 (ACK를 준다. ECU 혼자 있는 버스에서 쓴다)",
    "#   ping [kbps=500]        일반 모드로 요청 한 번(01 00). ACK가 오면 속도·결선이 맞다",
    "#   req <바이트...>        읽기 요청 하나. 예: req 21 21, req 01 05, req 03",
    "#   rpm                    회전수 요청(rpmsrc) 하나",
    "#   rpmsrc [바이트... [div N] | none]  회전수 요청 보기·바꾸기. 기본 01 0C / 4. 예: rpmsrc 21 21 (KTM 390 2016)",
    "#   pids                   표준 항목(서비스 01) 지원 목록을 묻고 하나씩 읽기",
    "#   dtc                    고장 코드 03·07·0A를 차례로 읽기(DTC 줄)",
    "#   scan21 [시작=00] [끝=FF]  서비스 21 번호 훑기(읽기만). ECU 회전수가 0일 때만, 8개마다 회전수·고장 코드 재확인",
    "#   poll <ms> <횟수> <바이트...>  같은 요청 반복. 예: poll 50 200 21 21. ms=0 이면 응답을 받자마자 다음 요청",
    "#                          여러 요청을 돌아가며: poll 0 300 21 21 / 01 11 / 21 21 / 01 0B",
    "#                          첫 요청을 주로, 나머지는 N번째마다: poll 20 15000 21 21 / 01 05 every 50",
    "#   (명령 끝에) force      watch·ping·req·poll 끝에 붙인다. 듣기에서 버스 오류가 보여도 500 kbps면 들어간다",
    "#   target <ID>            요청 ID 바꾸기(기본 7DF, ECU 지정은 7E0)",
    "#   speed <kbps>           요청 명령이 쓸 속도(auto·listen은 이 값을 바꾸지 않는다)",
    "#   print on|off           받은 프레임을 줄마다 찍기",
    "#   loopback bench         책상 전용 자체 시험(조용한 버스에서만, 자기 수신으로 한 번 송신)",
};

static void help(void)
{
    sct_outf("# 순정 ECU CAN 읽기 도구 v%s. 명령 (숫자는 kbps·초, 바이트는 16진):\n", SCT_VERSION);
    for (size_t i = 0; i < sizeof k_help / sizeof k_help[0]; i++) sct_outf("%s\n", k_help[i]);
    port_board_help();
    sct_outf("#   hello / status / close / help\n");
    sct_outf("# 읽기 서비스(%s)만 보낸다. 보내기 전에 같은 속도로 0.3초 듣고, 버스 오류가 보이면 보내지 않는다.\n", SCT_ALLOWED_TEXT);
    sct_outf("# 명령이 도는 동안 들어온 입력은 '멈춤'으로 보고 버린다. 다음 명령은 # done 을 본 뒤 보낸다.\n");
}

static void bad_args(const char *usage)
{
    sct_outf("# %s\n", usage);
    err_line(SCT_E_BAD_ARGS);
}

// 요청 바이트 해석. 1~7바이트, 각 16진 1~2자리
static int parse_req(char **tok, int from, int to, uint8_t *req)
{
    int n = 0;
    for (int i = from; i < to; i++) {
        if (n >= 7) return -2;
        if (!cli_parse_hex_byte(tok[i], &req[n])) return -1;
        n++;
    }
    return n;
}

static void cmd_rpmsrc(char **tok, int nt)
{
    char src[40];
    if (nt == 2 && !strcmp(tok[1], "none")) {
        s_rpm.on = false;
    } else if (nt > 1) {
        int end = nt, d = 4;
        if (nt >= 4 && !strcmp(tok[nt - 2], "div")) {
            if (!cli_parse_int(tok[nt - 1], 1, 1000, &d)) {
                bad_args("div는 1~1000");
                return;
            }
            end = nt - 2;
        }
        uint8_t req[7] = {0};
        int n = parse_req(tok, 1, end, req);
        if (n < 1) {
            bad_args("예: rpmsrc 01 0C, rpmsrc 21 21, rpmsrc 22 F4 0C div 4, rpmsrc none");
            return;
        }
        if (!policy_service_allowed(req[0])) {
            sct_outf("# 읽기 서비스(%s)만 회전수 요청으로 쓸 수 있다\n", SCT_ALLOWED_TEXT);
            err_line(SCT_E_BLOCKED);
            return;
        }
        memcpy(s_rpm.req, req, sizeof req);
        s_rpm.n = n;
        s_rpm.div = d;
        s_rpm.on = true;
    }
    rpm_src_text(src, sizeof src);
    sct_outf("# rpmsrc %s\n", src);
}

static void cmd_poll_line(char **tok, int nt)
{
    static uint8_t reqs[POLL_REQS][7];
    int lens[POLL_REQS] = {0}, nreq = 0, n = 0, every = 0;
    bool bad = false, badhex = false;
    if (nt > 5 && !strcmp(tok[nt - 2], "every")) { // 끝의 "every N"
        if (!cli_parse_int(tok[nt - 1], 2, POLL_MAX_COUNT, &every)) bad = true;
        nt -= 2;
    }
    for (int i = 3; i < nt && !bad; i++) {
        if (!strcmp(tok[i], "/")) { // 다음 요청
            if (!n || nreq + 1 >= POLL_REQS) bad = true;
            else lens[nreq++] = n;
            n = 0;
        } else if (n < 7) {
            if (!cli_parse_hex_byte(tok[i], &reqs[nreq][n])) badhex = bad = true;
            else n++;
        } else bad = true;
    }
    if (n && !bad) lens[nreq++] = n;
    else bad = true;
    int ms = -1, cnt = 0;
    if (nt < 3 || !cli_parse_int(tok[1], 0, 60000, &ms) || !cli_parse_int(tok[2], 1, POLL_MAX_COUNT, &cnt)) bad = true;
    if (bad || nreq < 1 || ms < 0 || (ms > 0 && ms < POLL_MIN_PERIOD_MS)) {
        sct_outf("# 예: poll 50 200 21 21 (간격 5 ms 이상, 횟수 40000 이하). poll 0 300 21 21 = 응답을 받자마자 다음 요청.\n");
        sct_outf("#     여러 요청을 돌아가며: poll 0 300 21 21 / 01 11 / 21 21 / 01 0B (8가지까지). 첫 요청을 주로, 나머지는 N번째마다: poll 20 15000 21 21 / 01 05 every 50\n");
        err_line(badhex ? SCT_E_BAD_HEX : SCT_E_BAD_ARGS);
        return;
    }
    for (int q = 0; q < nreq; q++) // 막은 서비스는 반복 전에 거른다(v1은 반복마다 실패로 셌다)
        if (!policy_service_allowed(reqs[q][0])) {
            explain_fail(SCT_BLOCKED, reqs[q]);
            return;
        }
    cmd_poll(ms, cnt, reqs, lens, nreq, every);
}

static void dispatch(char **tok, int nt)
{
    const char *c = tok[0];
    if (!strcmp(c, "help") || !strcmp(c, "?")) help();
    else if (!strcmp(c, "hello")) cli_banner();
    else if (!strcmp(c, "auto")) cmd_auto();
    else if (!strcmp(c, "listen") || !strcmp(c, "watch")) {
        int kbps = 0, sec = 30;
        if (nt < 2) {
            sct_outf("# 속도를 적는다. 예: %s 500\n", c);
            err_line(SCT_E_BAD_ARGS);
        } else if (!cli_parse_int(tok[1], 0, 100000, &kbps) || !policy_speed_ok(kbps)) {
            sct_outf("# 지원 속도: 1000 500 250 125 100 (kbps)\n");
            err_line(SCT_E_BAD_SPEED);
        } else if (nt > 2 && !cli_parse_int(tok[2], 1, LISTEN_MAX_SEC, &sec)) bad_args("초는 1~86400");
        else cmd_listen(kbps, sec, c[0] == 'w');
    } else if (!strcmp(c, "ping")) {
        int kbps = SCT_CONFIRMED_KBPS;
        if (nt > 1 && !cli_parse_int(tok[1], 0, 100000, &kbps)) kbps = 0; // 숫자가 아니면 cmd_ping이 속도 오류를 낸다
        cmd_ping(kbps);
    } else if (!strcmp(c, "rpmsrc")) cmd_rpmsrc(tok, nt);
    else if (!strcmp(c, "req") || !strcmp(c, "rpm")) {
        uint8_t req[7] = {0}, rsp[RSP_CAP];
        int n = s_rpm.n;
        memcpy(req, s_rpm.req, sizeof req);
        if (c[1] == 'e') n = parse_req(tok, 1, nt, req);
        else if (!s_rpm.on) n = 0;
        if (n == -1) {
            sct_outf("# 잘못된 16진: 바이트는 16진 1~2자리로 적는다. 예: req 21 21\n");
            err_line(SCT_E_BAD_HEX);
        } else if (n == -2) bad_args("요청은 7바이트까지다(길이 바이트는 적지 않는다)");
        else if (n < 1) bad_args("예: req 21 21");
        else {
            diag_result_t res;
            do_req(req, n, rsp, sizeof rsp, true, &res);
        }
    } else if (!strcmp(c, "pids")) cmd_pids();
    else if (!strcmp(c, "dtc")) cmd_dtc();
    else if (!strcmp(c, "scan21")) {
        uint8_t a = 0x00, b = 0xFF;
        if ((nt > 1 && !cli_parse_hex_byte(tok[1], &a)) || (nt > 2 && !cli_parse_hex_byte(tok[2], &b)) || a > b) {
            sct_outf("# 범위는 00~FF\n");
            err_line(SCT_E_BAD_ARGS);
        } else cmd_scan21(a, b);
    } else if (!strcmp(c, "poll")) cmd_poll_line(tok, nt);
    else if (!strcmp(c, "target")) {
        uint32_t id = 0;
        if (nt < 2 || !cli_parse_hex_id(tok[1], &id) || !policy_req_id_ok(id)) {
            sct_outf("# 요청 ID는 7DF 또는 7E0~7E7\n");
            err_line(SCT_E_BAD_TARGET);
        } else {
            diag_set_target(id);
            sct_outf("# 요청 ID = %" PRIX32 "\n", id);
        }
    } else if (!strcmp(c, "speed")) {
        int k = 0;
        if (nt > 1 && cli_parse_int(tok[1], 0, 100000, &k) && policy_speed_ok(k)) {
            busguard_close();
            diag_set_speed(k);
            sct_outf("# 요청 속도 = %d kbps\n", k);
        } else {
            sct_outf("# 지원 속도: 1000 500 250 125 100\n");
            err_line(SCT_E_BAD_SPEED);
        }
    } else if (!strcmp(c, "print")) {
        sct_out_set_print(!(nt > 1 && !strcmp(tok[1], "off")));
        sct_outf("# 프레임 줄 출력 %s\n", sct_out_print_enabled() ? "켬" : "끔");
    } else if (!strcmp(c, "status")) {
        sct_outf("# 요청 속도 %d kbps, 요청 ID %" PRIX32 ". 드라이버는 명령이 끝날 때마다 내린다\n", diag_speed(), diag_target());
        port_board_status();
        busguard_print_status();
    } else if (!strcmp(c, "close")) {
        busguard_close();
        sct_outf("# 닫음\n");
    } else if (!strcmp(c, "loopback")) cmd_loopback(nt, tok);
    else {
        busguard_close(); // 보드 명령(selftest 등)이 드라이버를 직접 만질 수 있다
        if (!port_board_cmd(nt, tok)) {
            sct_outf("# 모르는 명령. help\n");
            err_line(SCT_E_UNKNOWN_CMD);
        }
    }
}

// 명령 끝에 일반 모드였으면 잠깐 더 ACK를 주며 받는다. 끊긴 여러 프레임 응답의 나머지 CF나 늦게 온 응답이
// 받아 줄 장치 없이 남으면, ECU가 ACK 오류를 내며 같은 프레임을 다시 보내 버스를 오류로 채운다(다음 명령의
// 송신 전 듣기도 그 오류 때문에 거절된다). 가상 버스에서 이 일이 보여 v2에서 넣었다.
static void linger_ack(void)
{
    if (!busguard_is_normal()) return;
    sct_frame_t f;
    int64_t t_end = port_now_us() + (int64_t)LINGER_MS * 1000;
    while (port_now_us() < t_end)
        if (port_can_recv(&f, 5) && sct_out_print_enabled()) sct_out_frame("RX", &f);
}

static void finish_command(void)
{
    linger_ack();
    busguard_close(); // 명령이 끝나면 드라이버를 내린다: 다음 송신은 다시 듣기 확인부터
    uint32_t dropped = sct_out_take_dropped();
    if (dropped) sct_outf("# 출력이 밀려 RX 줄 %" PRIu32 "개를 버렸다(기록에 없다). print off 로 줄이거나 listen 의 ID 표를 본다\n", dropped);
    sct_out_flush(); // "# done"은 앞의 줄이 다 나간 뒤에
    sct_outf("# done\n");
    sct_out_flush();
}

void cli_exec(char *line)
{
    char *tok[CLI_MAX_TOKENS];
    int nt = 0;
    bool too_many = false;
    for (char *p = strtok(line, " \t,"); p; p = strtok(NULL, " \t,")) {
        if (nt < CLI_MAX_TOKENS) tok[nt++] = p;
        else too_many = true;
    }
    if (!nt) return;
    for (char *p = tok[0]; *p; p++) *p = (char)tolower((unsigned char)*p);
    port_led(SCT_LED_ERR, false);
    // 명령 끝의 force: 송신 전 듣기의 버스 오류 거부를 건너뛴다(확인된 속도에서만). 명령마다 다시 적어야 한다
    busguard_set_force(false);
    if (nt > 1 && !strcmp(tok[nt - 1], "force")) {
        busguard_set_force(true);
        nt--;
    }
    if (too_many) bad_args("낱말이 너무 많다");
    else dispatch(tok, nt);
    busguard_set_force(false);
    finish_command();
}

void cli_init(void)
{
    diag_init();
    s_rpm = (rpm_src_t){{0x01, 0x0C}, 2, 4.0, true};
    busguard_set_force(false);
    busguard_close();
    sct_out_set_print(true);
}

void cli_banner(void)
{
    sct_outf("HELLO,stock-can-tool,%s,%s,%d,%s\n", SCT_VERSION, port_board_name(), SCT_LINE_FORMAT, cli_build_date());
    port_board_banner();
}

bool cli_step(void)
{
    char line[CLI_LINE_MAX];
    int n = 0;
    bool overflow = false;
    sct_out_raw("> ", 2);
    sct_out_flush();
    for (;;) {
        int c = port_con_getc(50);
        if (c == SCT_CON_EOF) return false;
        if (c < 0) continue;
        if (c == '\t') c = ' '; // v1은 탭을 버려 "req<탭>21"이 "req21"이 됐다
        if (c == '\r' || c == '\n') {
            if (!n && !overflow) continue;
            line[n] = 0;
            sct_outf("\n");
            if (overflow) {
                sct_outf("# 줄이 너무 길다(%d자 넘음). 통째로 버렸다\n", CLI_LINE_MAX - 1);
                err_line(SCT_E_LINE_TOO_LONG);
                finish_command();
                return true;
            }
            cli_exec(line);
            return true;
        }
        if ((c == 8 || c == 127) && n) {
            n--;
            continue;
        }
        if (c >= 32 && c < 127) {
            if (n < CLI_LINE_MAX - 1) line[n++] = (char)c;
            else overflow = true;
        }
    }
}
