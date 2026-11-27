// 가상 버스와 도구 쪽 CAN 컨트롤러(port_can_*), 가상 시계(port_now_us, port_sleep_ms).
// 모형의 단순화(가정):
//   - 중재(arbitration)는 없다. 도구와 ECU가 같은 때 보내도 겹치지 않는다
//   - 프레임 시간 = 최악 스터핑 비트 수 ÷ 속도. 프레임 사이 간격(3비트)은 오류 프레임 때만 넣는다
//   - 듣기 전용 노드는 ACK도 오류 표시도 내지 않는다. 속도가 다른 프레임은 버스 오류 1건으로 센다
//   - 오류 카운터: ACK 없는 단발 송신마다 TEC +8, 성공하면 −1 [표준 ISO 11898-1의 규칙을 줄인 것]
//   - 드라이버를 다시 열면 카운터는 0(ESP-IDF TWAI 드라이버를 다시 설치하는 것과 같다)
#include <inttypes.h>
#include <string.h>
#include "sct/cli.h"
#include "sct/out.h"
#include "sct/policy.h"
#include "sim.h"

#define RXQ_LEN 256 // v1 rx_queue_len과 같다

static struct {
    int64_t vt;
    bool talk;
    // 도구 쪽 컨트롤러
    bool open;
    sct_mode_t mode;
    int kbps;
    sct_bus_status_t st;
    sct_frame_t rxq[RXQ_LEN];
    int rx_head, rx_n;
    uint32_t tool_tx;
    // 고장 주입
    uint32_t inj_bus_errors;  // 드라이버를 열 때마다 버스 오류가 이만큼 보인다(잡음·속도 다른 장치)
    uint32_t inj_tec, inj_rec; // 일반 모드로 열 때 카운터를 이 값으로
    int inj_txfail;            // 다음 송신 한 번: 0 없음, 1 ACK 없음, 2 bus-off, 3 드라이버 오류
} b;

int64_t sim_time_us(void) { return b.vt; }
uint32_t sim_violations(void) { return ecu_violations(); }
uint32_t sim_requests(void) { return ecu_requests(); }
uint32_t sim_tool_tx(void) { return b.tool_tx; }
void sim_set_talk(bool on) { b.talk = on; }

void sim_reset(void)
{
    memset(&b, 0, sizeof b);
    b.vt = SIM_BOOT_US;
    b.talk = true;
    ecu_reset();
}

static void rx_push(const sct_frame_t *f)
{
    if (b.rx_n >= RXQ_LEN) {
        b.st.rx_missed++;
        return;
    }
    b.rxq[(b.rx_head + b.rx_n) % RXQ_LEN] = *f;
    b.rx_n++;
}

// ECU가 낸 프레임 하나가 버스에서 끝났다. 받는 쪽을 정하고 ACK가 있었는지 돌려준다
static bool deliver(const sct_frame_t *f)
{
    int ek = ecu_kbps();
    bool tool_same = b.open && b.kbps == ek;
    bool acked = ecu_other_acker() || (tool_same && b.mode != SCT_MODE_LISTEN);
    if (b.open && !tool_same) { // 다른 속도의 프레임: 도구는 오류로 본다
        b.st.bus_errors++;
        if (b.mode == SCT_MODE_NORMAL && b.st.rec < 255) b.st.rec++;
        return acked;
    }
    if (tool_same) {
        if (acked) rx_push(f);
        else b.st.bus_errors++; // 아무도 ACK를 주지 않아 ECU가 오류 표시를 낸다. 듣기 전용 쪽에는 오류만 보인다
    }
    return acked;
}

static void step_event(int64_t te)
{
    if (te > b.vt) b.vt = te;
    sct_frame_t f;
    if (ecu_pop_event(te, &f)) ecu_tx_result(deliver(&f), b.vt);
}

static void advance_to(int64_t t)
{
    for (;;) {
        int64_t te = ecu_next_event();
        if (te > t) break;
        step_event(te);
    }
    if (t > b.vt) b.vt = t;
}

int64_t port_now_us(void) { return b.vt; }

void port_sleep_ms(int ms)
{
    if (ms > 0) advance_to(b.vt + (int64_t)ms * 1000);
}

bool port_can_open(int kbps, sct_mode_t mode)
{
    if (!policy_speed_ok(kbps)) return false;
    b.open = true;
    b.mode = mode;
    b.kbps = kbps;
    memset(&b.st, 0, sizeof b.st);
    b.rx_head = b.rx_n = 0;
    b.st.bus_errors = b.inj_bus_errors;
    if (mode == SCT_MODE_NORMAL) {
        b.st.tec = b.inj_tec;
        b.st.rec = b.inj_rec;
    }
    return true;
}

void port_can_close(void)
{
    b.open = false;
    b.rx_n = 0;
}

bool port_can_status(sct_bus_status_t *s)
{
    if (!b.open) return false;
    *s = b.st;
    s->err_passive = !s->bus_off && (s->tec >= 128 || s->rec >= 128);
    return true;
}

sct_tx_result_t port_can_send_once(const sct_frame_t *f, int wait_ms, int64_t *t_txdone_us)
{
    (void)wait_ms;
    if (!b.open || b.mode == SCT_MODE_LISTEN || b.st.bus_off) return SCT_TX_DRIVER_ERR;
    int inj = b.inj_txfail;
    b.inj_txfail = 0;
    if (inj == 3) return SCT_TX_DRIVER_ERR;
    b.tool_tx++;
    advance_to(b.vt + sim_frame_us(f->dlc, b.kbps)); // 프레임이 버스를 차지하는 시간
    if (inj == 2) {
        b.st.bus_off = true;
        b.st.tec = 256;
        b.st.tx_failed++;
        return SCT_TX_BUS_OFF;
    }
    sct_frame_t g = *f;
    g.t_us = b.vt;
    bool ecu_hears = ecu_acks(b.kbps);
    bool acked = inj != 1 && (ecu_hears || (ecu_other_acker() && b.kbps == ecu_kbps()));
    if (b.mode == SCT_MODE_SELFTEST) {
        rx_push(&g); // 자기 수신 요청: ACK가 없어도 송신이 끝나고 자기 프레임을 되받는다
        if (ecu_hears && inj != 1) ecu_on_frame(&g, b.vt);
        if (t_txdone_us) *t_txdone_us = b.vt;
        return SCT_TX_ACKED;
    }
    if (!acked) {
        b.st.tec += 8;
        b.st.tx_failed++;
        b.st.bus_errors++;
        if (b.st.tec >= 256) {
            b.st.bus_off = true;
            return SCT_TX_BUS_OFF;
        }
        return SCT_TX_NO_ACK;
    }
    if (b.st.tec) b.st.tec--;
    if (ecu_hears) ecu_on_frame(&g, b.vt);
    if (t_txdone_us) *t_txdone_us = b.vt;
    return SCT_TX_ACKED;
}

bool port_can_recv(sct_frame_t *f, int timeout_ms)
{
    int64_t deadline = b.vt + (int64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000;
    for (;;) {
        if (b.open && b.rx_n) {
            *f = b.rxq[b.rx_head];
            b.rx_head = (b.rx_head + 1) % RXQ_LEN;
            b.rx_n--;
            f->t_us = b.vt;
            return true;
        }
        int64_t te = ecu_next_event();
        if (te > deadline) {
            if (deadline > b.vt) b.vt = deadline;
            return false;
        }
        step_event(te);
    }
}

bool port_talk_enabled(void) { return b.talk; }
void port_led(int which, bool on)
{
    (void)which;
    (void)on;
}
const char *port_board_name(void) { return "sim"; }

void port_board_help(void)
{
    sct_outf("#   sim ...                (가상 빌드만) 가상 ECU와 고장 주입. sim help\n");
}

void port_board_banner(void)
{
    sct_outf("# stock-can-tool v%s 가상 펌웨어(가상 버스 + 가상 ECU). 실제 차와 무관하다. help 로 명령 보기\n", SCT_VERSION);
}

void port_board_status(void)
{
    sct_outf("# 가상 시각 %.3f s, TALK %s\n", (double)b.vt / 1e6, b.talk ? "켜짐" : "꺼짐");
}

static void sim_help(void)
{
    sct_outf("# sim rpm <n> | kbps <속도> | noack on|off | delay <ms> | pending <횟수> [간격 ms]\n");
    sct_outf("# sim dropcf|cutcf|badsn|shortcf <k>   다음 여러 프레임 응답의 k번째 CF를 빠뜨림·끊음·순번 틀림·DLC 모자람\n");
    sct_outf("# sim stale <바이트...>   다음 응답 앞에 다른 요청의 늦은 응답 하나\n");
    sct_outf("# sim adddtc <P1234> [after <21 요청 수>] | stream <ID> <ms> [ack] | stream off\n");
    sct_outf("# sim buserr <n> | tec <n> | rec <n> | txfail noack|busoff|driver | talk on|off\n");
    sct_outf("# sim status | time | wait <ms> | reset\n");
}

bool sim_cmd(int argc, char **argv)
{
    if (argc < 2) {
        sim_help();
        return true;
    }
    const char *c = argv[1];
    int v = 0;
    if (!strcmp(c, "help")) sim_help();
    else if (!strcmp(c, "reset")) {
        bool talk = b.talk;
        sim_reset();
        b.talk = talk;
        sct_outf("SIM,reset\n");
    } else if (!strcmp(c, "time")) sct_outf("SIM,time,%.3f\n", (double)b.vt / 1000.0);
    else if (!strcmp(c, "wait") && argc > 2 && cli_parse_int(argv[2], 0, 3600000, &v)) {
        port_sleep_ms(v); // 가상 시간만 흐른다(드라이버가 내려가 있으면 ECU 프레임은 아무도 받지 않는다)
        sct_outf("SIM,wait,%d\n", v);
    }
    else if (!strcmp(c, "status")) {
        sct_outf("SIM,bus,t_ms=%.3f,tool_tx=%" PRIu32 ",buserr=%" PRIu32 ",tec=%" PRIu32 ",rec=%" PRIu32 ",talk=%d\n", (double)b.vt / 1000.0, b.tool_tx,
                 b.inj_bus_errors, b.inj_tec, b.inj_rec, b.talk ? 1 : 0);
        ecu_status_line();
    } else if (!strcmp(c, "buserr") && argc > 2 && cli_parse_int(argv[2], 0, 100000, &v)) {
        b.inj_bus_errors = (uint32_t)v;
        sct_outf("SIM,buserr,%d\n", v);
    } else if ((!strcmp(c, "tec") || !strcmp(c, "rec")) && argc > 2 && cli_parse_int(argv[2], 0, 255, &v)) {
        if (c[0] == 't') b.inj_tec = (uint32_t)v;
        else b.inj_rec = (uint32_t)v;
        sct_outf("SIM,%s,%d\n", c, v);
    } else if (!strcmp(c, "txfail") && argc > 2) {
        b.inj_txfail = !strcmp(argv[2], "noack") ? 1 : !strcmp(argv[2], "busoff") ? 2 : !strcmp(argv[2], "driver") ? 3 : 0;
        if (!b.inj_txfail) return false;
        sct_outf("SIM,txfail,%s\n", argv[2]);
    } else if (!strcmp(c, "talk") && argc > 2) {
        b.talk = !strcmp(argv[2], "on");
        sct_outf("SIM,talk,%d\n", b.talk ? 1 : 0);
    } else return ecu_cmd(argc - 1, argv + 1);
    return true;
}

bool port_board_cmd(int argc, char **argv)
{
    if (strcmp(argv[0], "sim") != 0) return false;
    if (!sim_cmd(argc, argv)) {
        sct_outf("# sim 명령을 알아듣지 못했다. sim help\n");
        sct_outf("ERR,%d,bad_args\n", 10);
    }
    return true;
}
