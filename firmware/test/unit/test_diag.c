// diag + busguard를 가상 버스·가상 ECU 위에서 시험한다
#include <string.h>
#include "../../port_sim/sim.h"
#include "console_capture.h"
#include "sct/busguard.h"
#include "sct/cli.h"
#include "sct/codes.h"
#include "sct/diag.h"
#include "sct/out.h"
#include "tmini.h"

static uint8_t g_rsp[256];
static diag_result_t g_res;

static void setup(void)
{
    cap_reset();
    sim_reset();
    sct_out_init(false);
    cli_init();
}

static void sim(const char *line)
{
    char buf[128], *argv[16];
    int argc = 0;
    snprintf(buf, sizeof buf, "%s", line);
    for (char *p = strtok(buf, " "); p && argc < 16; p = strtok(NULL, " ")) argv[argc++] = p;
    CHECK(sim_cmd(argc, argv));
}

static void req(const uint8_t *q, int n, int timeout_ms) { diag_request(q, n, g_rsp, sizeof g_rsp, timeout_ms, &g_res); }

static const uint8_t RPM[2] = {0x21, 0x21};

static void t_positive(void)
{
    setup();
    sim("sim rpm 2175");
    req(RPM, 2, 500);
    CHECK_EQ_INT(g_res.status, SCT_OK);
    CHECK_EQ_INT(g_res.len, 4);
    CHECK(g_rsp[0] == 0x61 && g_rsp[1] == 0x21 && (g_rsp[2] * 256 + g_rsp[3]) == 8700); // 2175 × 4
    CHECK_EQ_INT(g_res.rsp_id, 0x7E8);
    CHECK(!g_res.partial);
    CHECK(g_res.lat_us > 8000 && g_res.lat_us < 9000); // [가정] 가상 ECU 8 ms + 프레임 두 개
    CHECK(g_res.txdone_us > 0 && g_res.txdone_us < 1000);
    CHECK(busguard_is_normal()); // 성공하면 다음 요청을 위해 열어 둔다(명령 끝에 cli가 내린다)
    CHECK_EQ_INT(sim_violations(), 0);
}

static void t_multiframe(void)
{
    setup();
    static const uint8_t q[1] = {0x03};
    req(q, 1, 500);
    CHECK_EQ_INT(g_res.status, SCT_OK);
    CHECK_EQ_INT(g_res.len, 26);
    CHECK(g_rsp[0] == 0x43 && g_rsp[1] == 0x0C && g_rsp[25] == 0x17);
    CHECK_EQ_INT(sim_tool_tx(), 2); // 요청 + 흐름 제어
}

static void t_negative(void)
{
    setup();
    diag_set_target(0x7E0);
    static const uint8_t q[2] = {0x21, 0x99};
    req(q, 2, 500);
    CHECK_EQ_INT(g_res.status, SCT_OK);
    CHECK(g_res.len == 3 && g_rsp[0] == 0x7F && g_rsp[1] == 0x21 && g_rsp[2] == 0x12);
    diag_set_target(0x7DF);
    req(q, 2, 500); // 공용 주소로는 모르는 번호에 답하지 않는다 [가정]
    CHECK_EQ_INT(g_res.status, SCT_NO_RESPONSE);
}

static void t_late_other_response(void)
{
    setup();
    sim("sim stale 41 05 54"); // 앞 요청(01 05)의 늦은 응답이 먼저 온다
    req(RPM, 2, 500);
    CHECK_EQ_INT(g_res.status, SCT_OK);
    CHECK(g_rsp[0] == 0x61 && g_rsp[1] == 0x21); // 늦은 응답을 이 요청의 답으로 받지 않는다
    CHECK(cap_contains("RX,") && cap_contains(",7E8,8,03 41 05 54")); // 버리지 않고 RX 줄로 남긴다
}

static void t_pending(void)
{
    setup();
    sim("sim pending 2"); // 0.5초 간격 두 번 → 1초 뒤 답
    req(RPM, 2, 500);
    CHECK_EQ_INT(g_res.status, SCT_OK);
    CHECK_EQ_INT(g_res.pending78, 2);
    CHECK_EQ_INT(diag_pending_total(), 2);
    CHECK(g_res.lat_us > 1000000 && g_res.lat_us < 1100000);
}

static void t_pending_cap(void)
{
    setup();
    sim("sim pending 20"); // 0.5초마다 계속 기다리라고만 한다 → 요청 시점부터 5초에서 끊는다
    int64_t t0 = sim_time_us();
    req(RPM, 2, 500);
    int64_t dt = sim_time_us() - t0;
    CHECK_EQ_INT(g_res.status, SCT_NO_RESPONSE);
    CHECK(g_res.pending78 >= 9 && g_res.pending78 <= 11);
    CHECK(dt >= 5300000 && dt < 5300000 + 20000); // 송신 전 듣기 0.3초 + 5초 상한
}

static void t_pending_extend(void)
{
    setup();
    sim("sim pending 1 1900"); // 78 뒤 1.9초에 답: 2초 연장 안이라 받는다
    req(RPM, 2, 500);
    CHECK_EQ_INT(g_res.status, SCT_OK);
    setup();
    sim("sim pending 1 2100"); // 78 뒤 2.1초: 연장(2초)을 넘는다
    req(RPM, 2, 500);
    CHECK_EQ_INT(g_res.status, SCT_NO_RESPONSE);
}

static void t_no_ack(void)
{
    setup();
    sim("sim noack on");
    req(RPM, 2, 500);
    CHECK_EQ_INT(g_res.status, SCT_NO_ACK);
    CHECK(!busguard_is_open());  // 바로 내렸다
    CHECK_EQ_INT(sim_tool_tx(), 1); // 재전송 없음: 버스에 나간 것은 한 번
}

static void t_bus_off(void)
{
    setup();
    sim("sim txfail busoff");
    req(RPM, 2, 500);
    CHECK_EQ_INT(g_res.status, SCT_BUS_OFF);
    CHECK(!busguard_is_open());
    CHECK(cap_contains("[BUS-OFF]"));
}

static void t_err_passive(void)
{
    setup();
    sim("sim tec 130");
    req(RPM, 2, 500);
    CHECK_EQ_INT(g_res.status, SCT_ERR_PASSIVE);
    CHECK(!busguard_is_open());
    CHECK(cap_contains("# 멈춤: 오류 수동 상태"));
}

static void t_listen_errors_refuse(void)
{
    setup();
    sim("sim buserr 3");
    req(RPM, 2, 500);
    CHECK_EQ_INT(g_res.status, SCT_BUS_ERR_REFUSED);
    CHECK_EQ_INT(sim_tool_tx(), 0); // 아무것도 내보내지 않았다
    CHECK(cap_contains("송신 안 함"));
    CHECK(!busguard_is_open());
}

static void t_force(void)
{
    setup();
    sim("sim buserr 3");
    busguard_set_force(true);
    req(RPM, 2, 500);
    CHECK_EQ_INT(g_res.status, SCT_OK); // 500 kbps에서는 force로 들어간다
    CHECK(cap_contains("# force:"));
    setup();
    sim("sim buserr 3");
    sim("sim kbps 250");
    diag_set_speed(250);
    busguard_set_force(true);
    req(RPM, 2, 500);
    CHECK_EQ_INT(g_res.status, SCT_BUS_ERR_REFUSED); // 250에서는 force를 받지 않는다
    CHECK_EQ_INT(sim_tool_tx(), 0);
    CHECK(cap_contains("force 는 실차에서 확인된 속도"));
    busguard_set_force(false);
}

static void t_blocked_never_sent(void)
{
    setup();
    for (int s = 0; s <= 0xFF; s++) {
        uint8_t q[2] = {(uint8_t)s, 0x00};
        if (s == 0x01 || s == 0x02 || s == 0x03 || s == 0x07 || s == 0x09 || s == 0x0A || s == 0x1A || s == 0x21 || s == 0x22) continue;
        req(q, 2, 100);
        CHECK_EQ_INT(g_res.status, SCT_BLOCKED);
    }
    CHECK_EQ_INT(sim_tool_tx(), 0);
    CHECK_EQ_INT(sim_requests(), 0);
    static const uint8_t long8[8] = {0x21, 1, 2, 3, 4, 5, 6, 7};
    req(long8, 8, 100); // 8바이트 요청은 SF로 못 보낸다
    CHECK_EQ_INT(g_res.status, SCT_BLOCKED);
    req(long8, 0, 100);
    CHECK_EQ_INT(g_res.status, SCT_BLOCKED);
}

static void t_talk_off(void)
{
    setup();
    sim_set_talk(false);
    req(RPM, 2, 500);
    CHECK_EQ_INT(g_res.status, SCT_TALK_OFF);
    CHECK_EQ_INT(sim_tool_tx(), 0);
    sim_set_talk(true);
}

static void t_isotp_faults(void)
{
    static const uint8_t q[1] = {0x03};
    setup();
    sim("sim cutcf 2");
    req(q, 1, 500);
    CHECK(g_res.status == SCT_OK && g_res.partial && g_res.isotp_err == ISOTP_ERR_TIMEOUT_CR && g_res.len == 13 && g_res.total == 26);
    setup();
    sim("sim dropcf 2");
    req(q, 1, 500);
    CHECK(g_res.status == SCT_OK && g_res.partial && g_res.isotp_err == ISOTP_ERR_WRONG_SN && g_res.len == 13);
    setup();
    sim("sim badsn 1");
    req(q, 1, 500);
    CHECK(g_res.status == SCT_OK && g_res.partial && g_res.isotp_err == ISOTP_ERR_WRONG_SN && g_res.len == 6);
    setup();
    sim("sim shortcf 3");
    req(q, 1, 500);
    CHECK(g_res.status == SCT_OK && g_res.partial && g_res.isotp_err == ISOTP_ERR_BAD_LEN && g_res.len == 25);
    // 고장은 응답 하나에만 걸린다
    req(q, 1, 500);
    CHECK(g_res.status == SCT_OK && !g_res.partial && g_res.len == 26);
}

static void t_truncate_cap(void)
{
    setup();
    static const uint8_t q[1] = {0x03};
    diag_request(q, 1, g_rsp, 10, 500, &g_res); // 받는 버퍼가 응답보다 작다
    CHECK(g_res.status == SCT_OK && g_res.partial && g_res.len == 10 && g_res.total == 26 && g_res.isotp_err == ISOTP_ERR_NONE);
}

static void t_no_violation_full_session(void)
{
    // 명령 줄 전체를 돌려도 가상 ECU가 읽기 밖의 요청을 받은 적이 없다
    setup();
    char cmds[][64] = {"rpmsrc 21 21", "ping 500", "req 03", "req 07", "dtc", "pids", "scan21 01 66", "poll 0 50 21 21 / 01 05", "req 14", "req 2E 00 00", "req 04"};
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) cli_exec(cmds[i]);
    CHECK_EQ_INT(sim_violations(), 0);
    CHECK(sim_requests() > 100);
    CHECK(!busguard_is_open()); // 명령이 끝나면 늘 내려가 있다
}

void group_diag(void)
{
    t_run("diag_positive", t_positive);
    t_run("diag_multiframe_fc", t_multiframe);
    t_run("diag_negative_7E0_vs_7DF", t_negative);
    t_run("diag_late_other_response_kept_as_rx", t_late_other_response);
    t_run("diag_pending78_counted", t_pending);
    t_run("diag_pending78_5s_cap", t_pending_cap);
    t_run("diag_pending78_2s_extension", t_pending_extend);
    t_run("diag_no_ack_single_shot", t_no_ack);
    t_run("diag_bus_off", t_bus_off);
    t_run("diag_error_passive", t_err_passive);
    t_run("busguard_listen_errors_refuse", t_listen_errors_refuse);
    t_run("busguard_force_500_only", t_force);
    t_run("policy_blocked_never_transmitted", t_blocked_never_sent);
    t_run("diag_talk_off", t_talk_off);
    t_run("diag_isotp_faults_end_to_end", t_isotp_faults);
    t_run("diag_truncate_to_cap", t_truncate_cap);
    t_run("safety_no_violation_full_session", t_no_violation_full_session);
}
