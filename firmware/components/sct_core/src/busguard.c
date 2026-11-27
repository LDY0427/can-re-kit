// 버스 보호. 문구는 v1 main.c(enter_normal, print_status)와 같다. PC 쪽 측정 스크립트가 "송신 안 함",
// "# 상태: 버스오류 N"을 찾는다.
#include "sct/busguard.h"

#include <inttypes.h>
#include "sct/codes.h"
#include "sct/out.h"
#include "sct/policy.h"

static struct {
    bool open, normal, force;
    int kbps;
} s;

bg_pre_t busguard_pre_tx_verdict(uint32_t bus_errors, int kbps, bool force)
{
    if (!bus_errors) return BG_PRE_GO;
    // force: ECU가 혼자 프레임을 내는데 받아 줄 장치가 없으면 듣기 전용 쪽에는 버스 오류만 보인다.
    // 이때는 속도가 맞아도 검사에 걸리므로, 실차에서 확인된 속도에서만 건너뛴다.
    if (force && policy_force_ok(kbps)) return BG_PRE_GO_FORCED;
    return BG_PRE_REFUSE;
}

bool busguard_jam_verdict(bool normal, uint32_t frames_delta, uint32_t err_delta)
{
    // 일반 모드인데 1초 동안 프레임은 하나도 못 받고 오류만 늘면, 이 도구가 남의 프레임마다 오류 표시를 내고 있을 수 있다
    return normal && frames_delta == 0 && err_delta > BG_JAM_ERR_PER_SEC;
}

bg_state_t busguard_state_of(const sct_bus_status_t *st)
{
    if (st->bus_off) return BG_STATE_BUSOFF;
    if (st->err_passive || st->tec >= BG_ERR_PASSIVE_LIMIT || st->rec >= BG_ERR_PASSIVE_LIMIT) return BG_STATE_PASSIVE;
    return BG_STATE_OK;
}

const char *busguard_state_key(bg_state_t st)
{
    return st == BG_STATE_BUSOFF ? "busoff" : st == BG_STATE_PASSIVE ? "passive" : "ok";
}

void busguard_set_force(bool on) { s.force = on; }
bool busguard_force(void) { return s.force; }
bool busguard_is_open(void) { return s.open; }
bool busguard_is_normal(void) { return s.open && s.normal; }
int busguard_kbps(void) { return s.kbps; }

void busguard_close(void)
{
    port_led(SCT_LED_RX, false);
    port_can_close(); // 열려 있지 않아도 부른다: TX 핀을 풀업 입력으로 다시 둔다
    s.open = false;
    s.normal = false;
}

bool busguard_open(int kbps, sct_mode_t mode)
{
    busguard_close();
    if (!policy_speed_ok(kbps)) {
        sct_outf("# 지원 속도: 1000 500 250 125 100 (kbps)\n");
        return false;
    }
    if (!port_can_open(kbps, mode)) {
        sct_outf("# CAN 드라이버 시작 실패\n");
        port_can_close();
        return false;
    }
    s.open = true;
    s.normal = mode == SCT_MODE_NORMAL;
    s.kbps = kbps;
    return true;
}

uint32_t busguard_bus_errors(void)
{
    sct_bus_status_t st;
    return (s.open && port_can_status(&st)) ? st.bus_errors : 0;
}

int busguard_enter_normal(int kbps)
{
    if (!busguard_open(kbps, SCT_MODE_LISTEN)) return SCT_DRIVER_ERR;
    sct_frame_t f;
    uint32_t n = 0;
    int64_t t_end = port_now_us() + (int64_t)BG_PRE_LISTEN_MS * 1000;
    while (port_now_us() < t_end) {
        if (!port_can_recv(&f, 10)) continue;
        n++;
        if (sct_out_print_enabled()) sct_out_frame("RX", &f);
    }
    uint32_t e = busguard_bus_errors();
    switch (busguard_pre_tx_verdict(e, kbps, s.force)) {
    case BG_PRE_GO_FORCED:
        sct_outf("# force: %d kbps로 0.3초 들었더니 버스 오류 %" PRIu32 "건(받은 프레임 %" PRIu32 "개). 확인된 속도라 일반 모드로 들어간다\n", kbps, e, n);
        break;
    case BG_PRE_REFUSE:
        sct_outf("# 송신 안 함: %d kbps로 0.3초 들었더니 버스 오류 %" PRIu32 "건(받은 프레임 %" PRIu32 "개). 속도가 다르거나 H·L이 바뀌었을 수 있다\n", kbps, e, n);
        if (policy_force_ok(kbps))
            sct_outf("# ECU 혼자 프레임을 내보내고 받아 주는 장치가 없을 때도 이렇게 보인다. H·L 결선이 맞다면 명령 끝에 force 를 붙인다\n");
        else if (s.force)
            sct_outf("# force 는 실차에서 확인된 속도(%d kbps)에서만 받는다\n", SCT_CONFIRMED_KBPS);
        busguard_close();
        return SCT_BUS_ERR_REFUSED;
    case BG_PRE_GO:
        break;
    }
    if (!busguard_open(kbps, SCT_MODE_NORMAL)) return SCT_DRIVER_ERR;
    return 0;
}

int busguard_ensure_normal(int kbps)
{
    if (s.open && s.normal && s.kbps == kbps) return 0;
    return busguard_enter_normal(kbps);
}

int busguard_check(void)
{
    sct_bus_status_t st;
    if (!s.open || !s.normal || !port_can_status(&st)) return 0;
    switch (busguard_state_of(&st)) {
    case BG_STATE_BUSOFF:
        sct_outf("# 멈춤: BUS-OFF. 송신 오류가 쌓여 버스에서 떨어졌다\n");
        return SCT_BUS_OFF;
    case BG_STATE_PASSIVE:
        sct_outf("# 멈춤: 오류 수동 상태(송신오류카운터 %" PRIu32 ", 수신오류카운터 %" PRIu32 ", %d 이상). 이 도구가 버스를 방해하고 있을 수 있다\n",
                 st.tec, st.rec, BG_ERR_PASSIVE_LIMIT);
        return SCT_ERR_PASSIVE;
    case BG_STATE_OK:
        break;
    }
    return 0;
}

void busguard_print_status(void)
{
    sct_bus_status_t st;
    if (!s.open || !port_can_status(&st)) return;
    sct_outf("# 상태: 버스오류 %" PRIu32 ", 수신오류카운터 %" PRIu32 ", 송신오류카운터 %" PRIu32 ", 송신실패 %" PRIu32 ", 놓친프레임 %" PRIu32
             ", 중재패배 %" PRIu32 "%s\n",
             st.bus_errors, st.rec, st.tec, st.tx_failed, st.rx_missed, st.arb_lost, st.bus_off ? " [BUS-OFF]" : "");
    sct_outf("STAT,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%s\n", st.bus_errors, st.rec, st.tec, st.tx_failed,
             st.rx_missed, st.arb_lost, busguard_state_key(busguard_state_of(&st)));
}
