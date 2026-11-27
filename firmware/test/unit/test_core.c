// policy, busguard 판정, stats, dtc, out 시험
#include <math.h>
#include <string.h>
#include "console_capture.h"
#include "sct/busguard.h"
#include "sct/dtc.h"
#include "sct/out.h"
#include "sct/policy.h"
#include "sct/stats.h"
#include "tmini.h"

// ---------------------------------------------------------------- policy
static void t_policy_services(void)
{
    // 0x00~0xFF 전수: 허용은 정확히 이 9개뿐이다
    static const uint8_t allowed[] = {0x01, 0x02, 0x03, 0x07, 0x09, 0x0A, 0x1A, 0x21, 0x22};
    int n_allowed = 0;
    for (int s = 0; s <= 0xFF; s++) {
        bool want = memchr(allowed, s, sizeof allowed) != NULL;
        if (policy_service_allowed((uint8_t)s) != want) printf("  service %02X: %d\n", s, !want);
        CHECK(policy_service_allowed((uint8_t)s) == want);
        n_allowed += policy_service_allowed((uint8_t)s);
    }
    CHECK_EQ_INT(n_allowed, 9);
    // 막는 서비스는 이름이 붙어 있다(거절 이유를 말할 수 있게)
    static const uint8_t named[] = {0x04, 0x08, 0x10, 0x11, 0x14, 0x27, 0x28, 0x2E, 0x2F, 0x30, 0x31, 0x34, 0x35, 0x36, 0x37, 0x3B, 0x3D, 0x85};
    for (size_t i = 0; i < sizeof named; i++) CHECK(strcmp(policy_service_name(named[i]), "이름 없는 서비스") != 0);
    CHECK(!strcmp(policy_service_name(0xF0), "이름 없는 서비스"));
}

static void t_policy_ids_speeds(void)
{
    int n = 0;
    for (uint32_t id = 0; id <= 0x7FF; id++) {
        bool want = id == 0x7DF || (id >= 0x7E0 && id <= 0x7E7);
        CHECK(policy_req_id_ok(id) == want);
        n += policy_req_id_ok(id);
    }
    CHECK_EQ_INT(n, 9);
    CHECK(!policy_req_id_ok(0x7E8));
    CHECK(!policy_req_id_ok(0x1FFFFFFF));
    int speeds = 0;
    for (int k = 0; k <= 2000; k++) speeds += policy_speed_ok(k);
    CHECK_EQ_INT(speeds, 5);
    for (int k = 0; k <= 2000; k++) CHECK(policy_force_ok(k) == (k == 500));
}

// ---------------------------------------------------------------- busguard 판정
static void t_bg_pre(void)
{
    CHECK(busguard_pre_tx_verdict(0, 500, false) == BG_PRE_GO);
    CHECK(busguard_pre_tx_verdict(0, 250, true) == BG_PRE_GO);
    CHECK(busguard_pre_tx_verdict(1, 500, false) == BG_PRE_REFUSE); // 듣기에서 오류 하나라도 → 거절
    CHECK(busguard_pre_tx_verdict(1, 500, true) == BG_PRE_GO_FORCED);
    CHECK(busguard_pre_tx_verdict(1000, 500, true) == BG_PRE_GO_FORCED);
    CHECK(busguard_pre_tx_verdict(1, 250, true) == BG_PRE_REFUSE); // force는 500에서만
    CHECK(busguard_pre_tx_verdict(1, 1000, true) == BG_PRE_REFUSE);
    CHECK(busguard_pre_tx_verdict(1, 125, true) == BG_PRE_REFUSE);
    CHECK(busguard_pre_tx_verdict(1, 100, true) == BG_PRE_REFUSE);
}

static void t_bg_jam(void)
{
    CHECK(!busguard_jam_verdict(true, 0, 50));  // 50건까지는 둔다(v1: 50 초과)
    CHECK(busguard_jam_verdict(true, 0, 51));
    CHECK(!busguard_jam_verdict(true, 1, 1000)); // 프레임을 받고 있으면 방해로 보지 않는다
    CHECK(!busguard_jam_verdict(false, 0, 1000)); // 듣기 전용은 버스에 아무것도 내지 않는다
}

static void t_bg_state(void)
{
    sct_bus_status_t st = {0};
    CHECK(busguard_state_of(&st) == BG_STATE_OK);
    st.tec = 127;
    CHECK(busguard_state_of(&st) == BG_STATE_OK);
    st.tec = 128;
    CHECK(busguard_state_of(&st) == BG_STATE_PASSIVE);
    st.tec = 0;
    st.rec = 128;
    CHECK(busguard_state_of(&st) == BG_STATE_PASSIVE);
    st.rec = 0;
    st.err_passive = true;
    CHECK(busguard_state_of(&st) == BG_STATE_PASSIVE);
    st.bus_off = true;
    CHECK(busguard_state_of(&st) == BG_STATE_BUSOFF);
    CHECK(!strcmp(busguard_state_key(BG_STATE_PASSIVE), "passive"));
}

// ---------------------------------------------------------------- stats
static void t_stats_bits(void)
{
    CHECK_EQ_INT(stats_frame_bits(0), 47 + 8);       // ⌊33/4⌋ = 8
    CHECK_EQ_INT(stats_frame_bits(8), 47 + 64 + 24); // ⌊97/4⌋ = 24 → 135
    CHECK_EQ_INT(stats_frame_bits(3), 47 + 24 + 14); // ⌊57/4⌋ = 14
    CHECK_EQ_INT(stats_frame_bits(15), stats_frame_bits(8));
    // 500 kbps에서 8바이트 프레임 1,000개/초 = 135,000비트 → 27 %
    CHECK(fabs(stats_bus_load_pct(135000, 500, 1.0) - 27.0) < 1e-9);
    CHECK(stats_bus_load_pct(1, 0, 1.0) == 0);
}

static void t_stats_hist(void)
{
    CHECK_EQ_INT(stats_lat_bin(0), 0);
    CHECK_EQ_INT(stats_lat_bin(1999), 0);
    CHECK_EQ_INT(stats_lat_bin(2000), 1);
    CHECK_EQ_INT(stats_lat_bin(4999), 1);
    CHECK_EQ_INT(stats_lat_bin(5000), 2);
    CHECK_EQ_INT(stats_lat_bin(9999), 2);
    CHECK_EQ_INT(stats_lat_bin(10000), 3);
    CHECK_EQ_INT(stats_lat_bin(20000), 4);
    CHECK_EQ_INT(stats_lat_bin(50000), 5);
    CHECK_EQ_INT(stats_lat_bin(99999), 5);
    CHECK_EQ_INT(stats_lat_bin(100000), 6);
    CHECK_EQ_INT(stats_lat_bin(5000000), 6);
}

static stats_lat_t g_lat;

static void t_stats_quantiles(void)
{
    stats_lat_reset(&g_lat);
    int64_t m, p;
    CHECK(!stats_lat_quantiles(&g_lat, &m, &p));
    for (int i = 100; i >= 1; i--) stats_lat_add(&g_lat, (int64_t)i * 1000); // 1~100 ms, 거꾸로 넣는다
    CHECK(stats_lat_quantiles(&g_lat, &m, &p));
    CHECK_EQ_INT(m, 51000); // k/2 = 50번째(0부터) = 51 ms (v1과 같은 방법)
    CHECK_EQ_INT(p, 96000); // k·95/100 = 95번째 = 96 ms
    CHECK_EQ_INT(g_lat.min, 1000);
    CHECK_EQ_INT(g_lat.max, 100000);
    CHECK_EQ_INT(g_lat.hist[0], 1); // 1 ms
    CHECK_EQ_INT(g_lat.hist[6], 1); // 100 ms
    stats_lat_reset(&g_lat);
    stats_lat_add(&g_lat, 7000);
    CHECK(stats_lat_quantiles(&g_lat, &m, &p) && m == 7000 && p == 7000);
    stats_lat_reset(&g_lat);
    for (int i = 0; i < STATS_LAT_KEEP + 500; i++) stats_lat_add(&g_lat, 1000);
    CHECK_EQ_INT(g_lat.kept, STATS_LAT_KEEP); // 넘치지 않는다
    CHECK_EQ_INT(g_lat.n, STATS_LAT_KEEP + 500);
}

// 표본이 2,000개를 넘으면 앞부분만이 아니라 전체로 분위수를 낸다.
// [실측 11-18] 5분(15,000개) 구간에서 앞 2,000개만 쓴 값 5.59 ms와 전체 값 5.85 ms가 달랐다.
static void t_stats_quantiles_all_samples(void)
{
    int64_t m, p;
    stats_lat_reset(&g_lat);
    for (int i = 0; i < 2000; i++) stats_lat_add(&g_lat, 1004);  // 앞 2,000개: 1.00 ms 근처
    for (int i = 0; i < 13000; i++) stats_lat_add(&g_lat, 9567); // 뒤 13,000개: 9.56 ms 근처
    CHECK(stats_lat_quantiles(&g_lat, &m, &p));
    CHECK_EQ_INT(m, 9560); // 7,500번째 표본이 든 10 us 칸의 아래 끝. 앞부분만 썼다면 1,004가 나온다
    CHECK_EQ_INT(p, 9560);
    stats_lat_reset(&g_lat);
    for (int i = 0; i < 2900; i++) stats_lat_add(&g_lat, 500);
    for (int i = 0; i < 100; i++) stats_lat_add(&g_lat, 25000 + i); // 20 ms 이상은 마지막 칸 하나
    CHECK(stats_lat_quantiles(&g_lat, &m, &p));
    CHECK_EQ_INT(m, 500);
    CHECK_EQ_INT(p, 500); // 95 % = 2,850번째
    stats_lat_reset(&g_lat);
    for (int i = 0; i < 1000; i++) stats_lat_add(&g_lat, 500);
    for (int i = 0; i < 2000; i++) stats_lat_add(&g_lat, 25000 + i);
    CHECK(stats_lat_quantiles(&g_lat, &m, &p));
    CHECK_EQ_INT(m, 26999); // 마지막 칸에 걸리면 최대값을 돌려준다(칸 안의 위치는 모른다)
    stats_lat_reset(&g_lat);
    for (int i = 0; i < 2000; i++) stats_lat_add(&g_lat, 3333); // 딱 2,000개까지는 정렬한 정확한 값
    CHECK(stats_lat_quantiles(&g_lat, &m, &p) && m == 3333 && p == 3333);
}

static void t_stats_ids(void)
{
    stats_reset();
    // 280: 10 ms 주기에 ±1 ms 흔들림, 첫 바이트 계수기. 300: 한 번
    int64_t t = 1000000;
    const int jit[] = {0, 1000, -1000, 0, 500, -500, 0, 0, 0, 0};
    for (int i = 0; i < 10; i++) {
        sct_frame_t f = {.id = 0x280, .dlc = 8, .data = {(uint8_t)i, 0x12, 0x34}, .t_us = t + i * 10000 + jit[i]};
        stats_add(&f);
    }
    sct_frame_t g = {.id = 0x300, .dlc = 2, .data = {1, 2}, .t_us = t};
    stats_add(&g);
    CHECK_EQ_INT(stats_frames(), 11);
    CHECK_EQ_INT(stats_nids(), 2);
    const stats_id_t *r = stats_id_at(0);
    CHECK(r && r->id == 0x280 && r->n == 10);
    CHECK(fabs(r->per_mean_ms - 10.0) < 1e-9); // 처음 0 → 끝 90 ms, 주기 9개
    CHECK(fabs(r->per_min_ms - 8.0) < 1e-9);   // 11 → 19 ms
    CHECK(fabs(r->per_max_ms - 11.0) < 1e-9);  // 0 → 11 ms
    CHECK(stats_period_std_ms(r) > 0.5 && stats_period_std_ms(r) < 2.0);
    CHECK_EQ_INT(r->changed, 0x01); // 첫 바이트만 바뀌었다
    CHECK(r->first[0] == 0 && r->last[0] == 9);
    const stats_id_t *q = stats_id_at(1);
    CHECK(q && q->n == 1 && stats_period_std_ms(q) == 0);
    CHECK(stats_id_at(2) == NULL);
    CHECK_EQ_INT(stats_bits(), 10 * 135 + stats_frame_bits(2));
    // 표가 차도 넘치지 않는다
    stats_reset();
    for (uint32_t id = 0; id < STATS_MAX_IDS + 20; id++) {
        sct_frame_t f = {.id = id, .dlc = 1, .t_us = 0};
        stats_add(&f);
    }
    CHECK_EQ_INT(stats_nids(), STATS_MAX_IDS);
    CHECK_EQ_INT(stats_frames(), STATS_MAX_IDS + 20);
}

// ---------------------------------------------------------------- dtc
static void t_dtc_text(void)
{
    char b[6];
    dtc_text(0x0650, b);
    CHECK(!strcmp(b, "P0650"));
    dtc_text(0x1505, b);
    CHECK(!strcmp(b, "P1505"));
    dtc_text(0x4123, b);
    CHECK(!strcmp(b, "C0123"));
    dtc_text(0x8000, b);
    CHECK(!strcmp(b, "B0000"));
    dtc_text(0xC1AB, b);
    CHECK(!strcmp(b, "U01AB"));
    dtc_text(0xFFFF, b);
    CHECK(!strcmp(b, "U3FFF"));
}

static void t_dtc_decode(void)
{
    static const uint8_t r03[] = {0x43, 0x0C, 0x06, 0x50, 0x15, 0x06, 0x15, 0x02, 0x15, 0x05, 0x16, 0x06, 0x01,
                                  0x18, 0x16, 0x01, 0x04, 0x80, 0x00, 0x30, 0x01, 0x13, 0x16, 0x07, 0x01, 0x17};
    // [실측 11-17] 07: 이번 주행 코드 3개(12바이트), P1505가 더해진 뒤(14바이트). 개수 바이트는 0000 채움까지 센다
    static const uint8_t r07[] = {0x47, 0x05, 0x15, 0x02, 0x00, 0x00, 0x04, 0x80, 0x00, 0x30, 0x00, 0x00};
    static const uint8_t r07b[] = {0x47, 0x06, 0x15, 0x02, 0x00, 0x00, 0x15, 0x05, 0x04, 0x80, 0x00, 0x30, 0x00, 0x00};
    // 끝 00 00이 잘린 인용(개수 06, 실린 쌍 5): 관대한 처리 시험용. 이 ECU의 원본이 아니다
    static const uint8_t r07_cut[] = {0x47, 0x06, 0x15, 0x02, 0x00, 0x00, 0x15, 0x05, 0x04, 0x80, 0x00, 0x30};
    dtc_list_t l;
    char b[6];
    CHECK(dtc_decode(0x03, r03, sizeof r03, &l));
    CHECK_EQ_INT(l.n, 12);
    CHECK_EQ_INT(l.count_byte, 12);
    CHECK(!l.count_mismatch);
    dtc_text(l.codes[0], b);
    CHECK(!strcmp(b, "P0650"));
    dtc_text(l.codes[11], b);
    CHECK(!strcmp(b, "P0117"));
    CHECK(dtc_decode(0x07, r07, sizeof r07, &l));
    CHECK(l.n == 3 && l.count_byte == 5 && l.pairs == 5 && !l.count_mismatch);
    CHECK(l.codes[0] == 0x1502 && l.codes[1] == 0x0480 && l.codes[2] == 0x0030);
    CHECK(dtc_decode(0x07, r07b, sizeof r07b, &l));
    CHECK(l.n == 4 && l.count_byte == 6 && l.pairs == 6 && !l.count_mismatch);
    CHECK(l.codes[0] == 0x1502 && l.codes[1] == 0x1505 && l.codes[2] == 0x0480 && l.codes[3] == 0x0030);
    // 개수가 실린 쌍보다 많으면(잘린 응답) 실린 쌍을 모두 읽고 어긋났다고 표시
    CHECK(dtc_decode(0x07, r07_cut, sizeof r07_cut, &l));
    CHECK(l.n == 4 && l.count_mismatch);
    // 개수가 쌍보다 적으면 개수만큼
    static const uint8_t r_less[] = {0x43, 0x01, 0x01, 0x23, 0x04, 0x56};
    CHECK(dtc_decode(0x03, r_less, sizeof r_less, &l) && l.n == 1 && l.codes[0] == 0x0123 && l.count_mismatch);
    static const uint8_t r_none[] = {0x4A, 0x00};
    CHECK(dtc_decode(0x0A, r_none, sizeof r_none, &l) && l.n == 0 && !l.count_mismatch);
    static const uint8_t r_neg[] = {0x7F, 0x03, 0x11};
    CHECK(!dtc_decode(0x03, r_neg, sizeof r_neg, &l));
    CHECK(!dtc_decode(0x03, r07, sizeof r07, &l)); // 서비스가 다른 응답
    static const uint8_t r_odd[] = {0x43, 0x02, 0x01, 0x23, 0x04}; // 끝이 잘린 쌍은 읽지 않는다
    CHECK(dtc_decode(0x03, r_odd, sizeof r_odd, &l) && l.n == 1);
    static const uint8_t r_only[] = {0x43};
    CHECK(dtc_decode(0x03, r_only, sizeof r_only, &l) && l.n == 0 && l.count_byte == -1);
}

// ---------------------------------------------------------------- out
static void t_out_direct(void)
{
    cap_reset();
    sct_out_init(false);
    sct_outf("A,%d\n", 1);
    sct_frame_t f = {.id = 0x7E8, .dlc = 3, .data = {0x02, 0x41, 0x05}, .t_us = 1234567};
    sct_out_frame("RX", &f);
    CHECK(!strcmp(cap_text(), "A,1\nRX,1234.567,7E8,3,02 41 05\n"));
    cap_clear();
    sct_frame_t x = {.id = 0x18DAF110, .ext = true, .dlc = 0, .t_us = 5};
    sct_out_frame("RX", &x);
    CHECK(!strcmp(cap_text(), "RX,0.005,x18DAF110,0,\n"));
    cap_clear();
    uint8_t big[300];
    for (int i = 0; i < 300; i++) big[i] = (uint8_t)i;
    sct_out_hex("ANS,", big, sizeof big, "\n");
    CHECK_EQ_INT(cap_len(), 4 + 300 * 3 - 1 + 1);
    CHECK(!strncmp(cap_text(), "ANS,00 01 02", 12));
    CHECK(strstr(cap_text(), "3F 40 41") != NULL); // 64바이트 조각 경계에서 공백이 빠지지 않는다
}

static void t_out_ring(void)
{
    cap_reset();
    sct_out_init(true);
    cap_set_autodrain(false); // 출력 태스크가 멈춘 상황
    sct_frame_t f = {.id = 0x280, .dlc = 8, .t_us = 1000};
    int pushed = 0;
    for (int i = 0; i < 2000; i++) {
        f.data[0] = (uint8_t)i;
        sct_out_frame("RX", &f);
        pushed++;
    }
    uint32_t dropped = sct_out_take_dropped();
    CHECK(dropped > 0);                          // 버퍼가 차면 RX 줄을 버린다
    CHECK(sct_out_pending() <= SCT_OUT_RING_SIZE);
    CHECK_EQ_INT(cap_len(), 0);                  // 아직 아무것도 안 나갔다
    CHECK_EQ_INT(sct_out_take_dropped(), 0);     // 센 값은 한 번 넘기면 0
    cap_set_autodrain(true);                     // 출력 태스크가 다시 돈다
    sct_outf("# done\n");                        // RX 아닌 줄은 기다렸다가 들어간다(버리지 않는다)
    sct_out_flush();
    CHECK_EQ_INT(sct_out_pending(), 0);
    CHECK(cap_count("RX,") == (int)(pushed - (int)dropped));
    const char *t = cap_text();
    size_t n = cap_len();
    CHECK(n >= 7 && !strcmp(t + n - 7, "# done\n")); // 순서가 지켜진다: done이 맨 끝
    // 첫 줄은 계수기 0, 둘째 줄은 1 (넣은 순서)
    CHECK(!strncmp(t, "RX,1.000,280,8,00 ", 18));
    sct_out_init(false);
}

static void t_out_ring_wrap(void)
{
    // 링 끝을 넘어가는 줄도 깨지지 않는다
    cap_reset();
    sct_out_init(true);
    cap_set_autodrain(true);
    char line[100];
    for (int i = 0; i < 1000; i++) {
        snprintf(line, sizeof line, "L,%04d,abcdefghijklmnopqrstuvwxyz\n", i);
        sct_outf("%s", line);
    }
    sct_out_flush();
    CHECK_EQ_INT(cap_count("L,"), 1000);
    CHECK(strstr(cap_text(), "L,0999,abcdefghijklmnopqrstuvwxyz\n") != NULL);
    CHECK(strstr(cap_text(), "L,0500,abcdefghijklmnopqrstuvwxyz\nL,0501,") != NULL);
    sct_out_init(false);
}

static void t_fmt(void)
{
    char b[32];
    sct_fmt_time(b, sizeof b, 0);
    CHECK(!strcmp(b, "0.000"));
    sct_fmt_time(b, sizeof b, 1308540);
    CHECK(!strcmp(b, "1308.540"));
    sct_fmt_time(b, sizeof b, -5);
    CHECK(!strcmp(b, "0.000"));
    uint8_t d[4] = {0x00, 0xAB, 0x0F, 0xF0};
    CHECK(sct_fmt_hex(b, sizeof b, d, 4) == 11 && !strcmp(b, "00 AB 0F F0"));
    CHECK(sct_fmt_hex(b, 6, d, 4) == 5 && !strcmp(b, "00 AB")); // 넘치면 자른다
    CHECK(sct_fmt_hex(b, 1, d, 4) == 0 && b[0] == 0);
}

void group_policy(void)
{
    t_run("policy_services_0x00_0xFF", t_policy_services);
    t_run("policy_request_ids_speeds_force", t_policy_ids_speeds);
}

void group_busguard(void)
{
    t_run("busguard_pre_tx_verdict", t_bg_pre);
    t_run("busguard_jam_verdict", t_bg_jam);
    t_run("busguard_error_state", t_bg_state);
}

void group_stats(void)
{
    t_run("stats_frame_bits_busload", t_stats_bits);
    t_run("stats_latency_bins", t_stats_hist);
    t_run("stats_quantiles_all_samples", t_stats_quantiles_all_samples);
    t_run("stats_median_p95", t_stats_quantiles);
    t_run("stats_id_table", t_stats_ids);
}

void group_dtc(void)
{
    t_run("dtc_text", t_dtc_text);
    t_run("dtc_decode_measured", t_dtc_decode);
}

void group_out(void)
{
    t_run("out_direct_format", t_out_direct);
    t_run("out_ring_drops_rx_only", t_out_ring);
    t_run("out_ring_wrap", t_out_ring_wrap);
    t_run("out_fmt", t_fmt);
}
