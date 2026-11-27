// ISO-TP 수신: 공유 벡터(test/vectors/isotp_vectors.json → test/build/isotp_vectors.h)와 몇 가지 직접 시험
#include <string.h>
#include "isotp_vectors.h"
#include "sct/isotp.h"
#include "tmini.h"

static isotp_rx_t g_rx; // 4 kB 버퍼라 스택에 두지 않는다

static const char *status_of(isotp_result_t r, const isotp_rx_t *c)
{
    if (r == ISOTP_R_COMPLETE) return "ok";
    if (r == ISOTP_R_ERROR) return isotp_err_key(c->err);
    return "none";
}

static int g_case;

static void run_vector(void)
{
    const vcase_t *v = &k_vcases[g_case];
    isotp_rx_init(&g_rx, 0x7E8, 0x7EF, NULL, NULL);
    isotp_rx_reset(&g_rx);
    const char *st = "none";
    int nfc = 0;
    bool fc_ok = true;
    bool done = false;
    for (int i = 0; i < v->nframes && !done; i++) {
        sct_frame_t f = {.id = v->frames[i].id, .dlc = v->frames[i].dlc};
        memcpy(f.data, v->frames[i].d, 8);
        isotp_action_t act;
        isotp_result_t r = isotp_rx_feed(&g_rx, &f, (int64_t)v->frames[i].t_ms * 1000, &act);
        if (act.send_fc) {
            if (nfc >= v->nfc || v->fc[nfc].after != i || v->fc[nfc].id != act.fc_id || memcmp(v->fc[nfc].d, act.fc_data, 8)) fc_ok = false;
            nfc++;
        }
        if (r == ISOTP_R_COMPLETE || r == ISOTP_R_ERROR) {
            st = status_of(r, &g_rx);
            done = true;
        }
    }
    if (!done && v->end_t_ms >= 0) {
        isotp_result_t r = isotp_rx_tick(&g_rx, (int64_t)v->end_t_ms * 1000);
        if (r == ISOTP_R_ERROR) st = status_of(r, &g_rx);
    }
    if (strcmp(st, v->status)) printf("  case %s: status %s, expected %s\n", v->name, st, v->status);
    CHECK(!strcmp(st, v->status));
    CHECK(fc_ok);
    CHECK_EQ_INT(nfc, v->nfc);
    if (strcmp(v->status, "none")) {
        CHECK_EQ_INT(g_rx.len, v->npayload);
        CHECK_EQ_INT(g_rx.total, v->total);
        CHECK(g_rx.len == v->npayload && !memcmp(g_rx.buf, v->payload, (size_t)v->npayload));
    }
}

static void t_make_sf(void)
{
    uint8_t out[8], p[7] = {1, 2, 3, 4, 5, 6, 7};
    CHECK(isotp_make_sf(p, 2, out));
    CHECK(out[0] == 2 && out[1] == 1 && out[2] == 2 && out[3] == 0 && out[7] == 0); // 남는 자리는 0x00
    CHECK(isotp_make_sf(p, 7, out) && out[0] == 7 && out[7] == 7);
    CHECK(!isotp_make_sf(p, 0, out));
    CHECK(!isotp_make_sf(p, 8, out));
}

static bool reject_all(void *u, const uint8_t *p, int n)
{
    (void)u;
    (void)p;
    (void)n;
    return false;
}

static void t_accept_filter(void)
{
    // 거른 FF에는 흐름 제어를 보내지 않는다(늦게 온 남의 여러 프레임 응답에 대고 송신하지 않는다)
    isotp_rx_init(&g_rx, 0x7E8, 0x7EF, reject_all, NULL);
    isotp_rx_reset(&g_rx);
    sct_frame_t ff = {.id = 0x7E8, .dlc = 8, .data = {0x10, 0x14, 0x62, 0xF1, 0x90, 0, 0, 0}};
    isotp_action_t act;
    CHECK(isotp_rx_feed(&g_rx, &ff, 0, &act) == ISOTP_R_NOT_MINE);
    CHECK(!act.send_fc);
    CHECK(!g_rx.receiving);
}

static void t_ext_rtr(void)
{
    isotp_rx_init(&g_rx, 0x7E8, 0x7EF, NULL, NULL);
    isotp_rx_reset(&g_rx);
    sct_frame_t f = {.id = 0x7E8, .dlc = 8, .ext = true, .data = {0x02, 0x41, 0x05}};
    CHECK(isotp_rx_feed(&g_rx, &f, 0, NULL) == ISOTP_R_NOT_MINE); // 29비트 ID는 응답이 아니다
    f.ext = false;
    f.rtr = true;
    CHECK(isotp_rx_feed(&g_rx, &f, 0, NULL) == ISOTP_R_NOT_MINE);
    f.rtr = false;
    f.dlc = 0;
    CHECK(isotp_rx_feed(&g_rx, &f, 0, NULL) == ISOTP_R_NOT_MINE);
}

void group_isotp(void)
{
    static char names[64][80];
    for (g_case = 0; g_case < k_nvcases; g_case++) {
        snprintf(names[g_case % 64], sizeof names[0], "isotp_vector_%s", k_vcases[g_case].name);
        t_run(names[g_case % 64], run_vector);
    }
    t_run("isotp_make_sf", t_make_sf);
    t_run("isotp_accept_filter_no_fc", t_accept_filter);
    t_run("isotp_ext_rtr_dlc0", t_ext_rtr);
}
