// 진단 요청-응답(SAE J1979 서비스 01~0A, ISO 14230-3 서비스 1A·21, 22). v1 diag_core·diag_request를 계층으로 나눴다.
//
// 송신은 단발(재전송 없음)이다. 이유: 속도가 틀리거나 H·L이 바뀐 채 자동 재전송을 켜 두면, ACK가 없는 동안
// 컨트롤러가 같은 프레임을 계속 다시 보내 버스를 붙잡는다. 단발이면 버스에 나가는 것은 시도 한 번이다.
#include "sct/diag.h"

#include <string.h>
#include "sct/busguard.h"
#include "sct/codes.h"
#include "sct/out.h"
#include "sct/policy.h"

static struct {
    uint32_t req_id;
    int req_kbps;
    int pending_total;
    const uint8_t *cur_req;
    int cur_n;
    isotp_rx_t rx;
} s = {.req_id = SCT_REQ_ID_FUNCTIONAL, .req_kbps = SCT_CONFIRMED_KBPS};

void diag_init(void)
{
    s.req_id = SCT_REQ_ID_FUNCTIONAL; // v1 기본: 공용 주소(이 차에서 답한 주소 [실측 11-16])
    s.req_kbps = SCT_CONFIRMED_KBPS;
    s.pending_total = 0;
}

void diag_set_target(uint32_t id) { s.req_id = id; }
uint32_t diag_target(void) { return s.req_id; }
void diag_set_speed(int kbps) { s.req_kbps = kbps; }
int diag_speed(void) { return s.req_kbps; }
int diag_pending_total(void) { return s.pending_total; }
void diag_pending_reset(void) { s.pending_total = 0; }

bool diag_rsp_matches(const uint8_t *req, int n, const uint8_t *d, int len)
{
    if (len >= 2 && d[0] == DIAG_NEG_RSP) return d[1] == req[0];
    if (len < 1 || d[0] != (uint8_t)(req[0] + DIAG_POS_OFFSET)) return false;
    if (n >= 2 && len >= 2 && d[1] != req[1]) return false;
    return true;
}

static bool accept_cb(void *user, const uint8_t *first, int n)
{
    (void)user;
    return diag_rsp_matches(s.cur_req, s.cur_n, first, n);
}

static int tx_status(sct_tx_result_t r)
{
    switch (r) {
    case SCT_TX_ACKED: return SCT_OK;
    case SCT_TX_NO_ACK: return SCT_NO_ACK;
    case SCT_TX_BUS_OFF: return SCT_BUS_OFF;
    default: return SCT_DRIVER_ERR;
    }
}

// 송신 실패 뒤: 상태를 찍고 바로 드라이버를 내린다(v1 diag_request와 같다). 다음 요청은 다시 듣기 확인부터.
static void stop_after_failure(void)
{
    busguard_print_status();
    busguard_close();
}

static void take_partial(diag_result_t *res, uint8_t *rsp, int cap, int64_t now, int64_t t0)
{
    int got = s.rx.len;
    int k = got < cap ? got : cap;
    memcpy(rsp, s.rx.buf, (size_t)k);
    res->status = SCT_OK; // v1처럼 받은 만큼을 응답으로 돌려준다. partial로 구별한다
    res->len = (uint16_t)k;
    res->total = s.rx.total;
    res->partial = true;
    res->isotp_err = s.rx.err;
    res->lat_us = now - t0;
}

void diag_request(const uint8_t *req, int n, uint8_t *rsp, int cap, int timeout_ms, diag_result_t *res)
{
    memset(res, 0, sizeof *res);
    res->txdone_us = -1;
    res->status = SCT_NO_RESPONSE;
    if (n < 1 || n > 7 || !policy_service_allowed(req[0])) {
        res->status = SCT_BLOCKED;
        return;
    }
    if (!port_talk_enabled()) {
        res->status = SCT_TALK_OFF;
        return;
    }
    int e = busguard_ensure_normal(s.req_kbps);
    if (e) {
        res->status = e;
        stop_after_failure();
        return;
    }
    sct_frame_t f;
    while (port_can_recv(&f, 0)) // 요청 전에 쌓여 있던 프레임
        if (sct_out_print_enabled()) sct_out_frame("RX", &f);

    sct_frame_t tx = {.id = s.req_id, .dlc = 8};
    isotp_make_sf(req, n, tx.data);
    s.cur_req = req;
    s.cur_n = n;
    isotp_rx_init(&s.rx, SCT_RSP_ID_FIRST, SCT_RSP_ID_LAST, accept_cb, NULL);
    isotp_rx_reset(&s.rx);

    int64_t t0 = port_now_us(), t_done = 0;
    int st = tx_status(port_can_send_once(&tx, DIAG_TX_WAIT_MS, &t_done));
    if (st != SCT_OK) {
        res->status = st;
        stop_after_failure();
        return;
    }
    res->txdone_us = t_done - t0;
    if ((e = busguard_check()) != 0) { // 송신 뒤 오류 카운터가 128을 넘었으면 더 머물지 않는다
        res->status = e;
        stop_after_failure();
        return;
    }

    int64_t t_end = t0 + (int64_t)timeout_ms * 1000;
    int64_t t_hard = t0 + (int64_t)DIAG_HARD_LIMIT_MS * 1000;
    for (;;) {
        int64_t now = port_now_us();
        if (now >= t_hard) break;
        if (!s.rx.receiving && now >= t_end) break;
        if (!port_can_recv(&f, DIAG_RECV_SLICE_MS)) {
            if (isotp_rx_tick(&s.rx, port_now_us()) == ISOTP_R_ERROR) {
                take_partial(res, rsp, cap, port_now_us(), t0);
                return;
            }
            continue;
        }
        isotp_action_t act;
        isotp_result_t ir = isotp_rx_feed(&s.rx, &f, f.t_us, &act);
        if (ir == ISOTP_R_NOT_MINE || ir == ISOTP_R_DISCARD) {
            // 다른 요청의 늦은 응답, 형식이 틀린 프레임, 남의 프레임. 버리지 않고 RX 줄로 남긴다
            if (sct_out_print_enabled()) sct_out_frame("RX", &f);
            if (isotp_rx_tick(&s.rx, f.t_us) == ISOTP_R_ERROR) {
                take_partial(res, rsp, cap, f.t_us, t0);
                return;
            }
            continue;
        }
        if (ir == ISOTP_R_IN_PROGRESS) {
            res->rsp_id = f.id;
            if (act.send_fc) {
                sct_frame_t fc = {.id = act.fc_id, .dlc = 8};
                memcpy(fc.data, act.fc_data, 8);
                int fst = tx_status(port_can_send_once(&fc, DIAG_TX_WAIT_MS, NULL));
                if (fst == SCT_OK) {
                    int chk = busguard_check();
                    if (chk) fst = chk;
                }
                if (fst != SCT_OK) { // 흐름 제어가 나가지 못했거나 오류 상태가 나빠졌다: 송신 실패와 같이 다룬다
                    res->status = fst;
                    stop_after_failure();
                    return;
                }
            }
            continue;
        }
        if (ir == ISOTP_R_ERROR) {
            res->rsp_id = f.id;
            take_partial(res, rsp, cap, f.t_us, t0);
            return;
        }
        // ISOTP_R_COMPLETE
        if (s.rx.len >= 3 && s.rx.buf[0] == DIAG_NEG_RSP && s.rx.buf[2] == DIAG_NRC_PENDING) {
            res->pending78++;
            s.pending_total++;
            int64_t ext = f.t_us + (int64_t)DIAG_PENDING_EXTEND_MS * 1000;
            t_end = ext < t_hard ? ext : t_hard;
            isotp_rx_reset(&s.rx);
            continue;
        }
        int k = s.rx.len < cap ? s.rx.len : cap;
        memcpy(rsp, s.rx.buf, (size_t)k);
        res->status = SCT_OK;
        res->rsp_id = f.id;
        res->len = (uint16_t)k;
        res->total = s.rx.len;
        res->partial = k < s.rx.len;
        res->lat_us = f.t_us - t0;
        return;
    }
    if (s.rx.receiving) { // 5초 상한에 걸렸을 때 여러 프레임을 받던 중이었다
        s.rx.receiving = false;
        s.rx.err = ISOTP_ERR_TIMEOUT_CR;
        s.rx.len = s.rx.got;
        take_partial(res, rsp, cap, port_now_us(), t0);
        return;
    }
    res->status = SCT_NO_RESPONSE;
}
