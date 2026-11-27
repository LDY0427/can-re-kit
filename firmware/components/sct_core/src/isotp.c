// ISO 15765-2 수신 상태 기계(일반 주소, 11비트, 고전 CAN 8바이트).
//
// 왜 포트에서 뗐나: v1은 재조립이 송신·수신 함수 안에 섞여 있어 실제 ECU 없이는 시험할 수 없었다.
// 여기서는 (프레임, 시각)만 받아 결과와 "흐름 제어를 보내라"는 요청을 돌려준다. 그래서 같은 시험 벡터
// (test/vectors/isotp_vectors.json)를 C와 파이썬이 같이 돌릴 수 있다.
//
// v1과 다른 점: 순번이 틀리면 v1은 그 프레임을 무시하고 계속 기다렸다(결국 시간 초과). 여기서는 바로 끝내고
// WRONG_SN을 알린다. 순번이 한 번 어긋나면 그 뒤 데이터는 어디에 붙여야 할지 알 수 없기 때문이다.
#include "sct/isotp.h"

#include <string.h>

#define PCI_SF 0x0
#define PCI_FF 0x1
#define PCI_CF 0x2
#define PCI_FC 0x3
#define SF_MAX_LEN 7       // [표준] 고전 CAN의 SF 최대 데이터 길이
#define FF_MIN_LEN 8       // [표준] FF 길이가 7 이하면 SF로 보냈어야 한다. 버린다
#define FF_DATA_BYTES 6    // FF에 실리는 데이터
#define CF_DATA_BYTES 7    // CF 하나에 실리는 데이터
#define FC_CTS 0x30        // [표준] 흐름 제어: 계속 보내라(ContinueToSend)
#define FC_BLOCK_SIZE 0x00 // 블록 제한 없음
#define FC_STMIN 0x00      // 프레임 간격 0. ECU가 낼 수 있는 가장 빠른 속도로 받는다

void isotp_rx_init(isotp_rx_t *c, uint32_t id_lo, uint32_t id_hi, isotp_accept_fn accept, void *user)
{
    memset(c, 0, sizeof *c);
    c->id_lo = id_lo;
    c->id_hi = id_hi;
    c->n_cr_us = ISOTP_N_CR_US;
    c->accept = accept;
    c->accept_user = user;
}

void isotp_rx_reset(isotp_rx_t *c)
{
    c->receiving = false;
    c->total = c->got = 0;
    c->next_sn = 1;
    c->err = ISOTP_ERR_NONE;
    c->len = 0;
}

static bool accepted(isotp_rx_t *c, const uint8_t *p, int n) { return !c->accept || c->accept(c->accept_user, p, n); }

static isotp_result_t fail(isotp_rx_t *c, isotp_err_t e)
{
    c->receiving = false;
    c->err = e;
    c->len = c->got; // 받은 만큼은 돌려준다(PARTIAL)
    return ISOTP_R_ERROR;
}

isotp_result_t isotp_rx_feed(isotp_rx_t *c, const sct_frame_t *f, int64_t now_us, isotp_action_t *act)
{
    if (act) act->send_fc = false;
    if (f->ext || f->rtr || f->id < c->id_lo || f->id > c->id_hi || f->dlc < 1) return ISOTP_R_NOT_MINE;
    uint8_t dlc = f->dlc > 8 ? 8 : f->dlc;
    uint8_t pci = f->data[0] >> 4;

    if (pci == PCI_SF) {
        if (c->receiving && f->id != c->rx_id) return ISOTP_R_NOT_MINE; // 다른 ECU의 응답. 받던 것은 그대로
        int len = f->data[0] & 0x0F;
        if (len < 1 || len > SF_MAX_LEN || dlc < len + 1) return ISOTP_R_DISCARD;
        if (!accepted(c, f->data + 1, len)) return ISOTP_R_NOT_MINE;
        if (c->receiving) c->restarts++; // [표준] 받는 중 같은 ID의 SF: 앞의 수신을 버리고 새 응답으로 본다
        c->receiving = false;
        memcpy(c->buf, f->data + 1, (size_t)len);
        c->len = (uint16_t)len;
        c->total = (uint16_t)len;
        c->got = (uint16_t)len;
        c->err = ISOTP_ERR_NONE;
        return ISOTP_R_COMPLETE;
    }

    if (pci == PCI_FF) {
        if (c->receiving && f->id != c->rx_id) return ISOTP_R_NOT_MINE; // 다른 ID의 FF는 받는 중이면 무시한다
        if (dlc < 8) return ISOTP_R_DISCARD;                              // [표준] FF는 DLC 8
        int tl = ((f->data[0] & 0x0F) << 8) | f->data[1];
        if (tl == 0 || tl < FF_MIN_LEN) return ISOTP_R_DISCARD; // 0 = 32비트 길이 형식(지원 안 함), 8 미만 = 형식 위반
        if (!accepted(c, f->data + 2, FF_DATA_BYTES)) return ISOTP_R_NOT_MINE;
        if (c->receiving) c->restarts++; // 같은 ID의 새 FF: 앞의 수신을 버리고 새로 시작
        c->receiving = true;
        c->rx_id = f->id;
        c->total = (uint16_t)tl;
        memcpy(c->buf, f->data + 2, FF_DATA_BYTES);
        c->got = FF_DATA_BYTES;
        c->next_sn = 1;
        c->deadline_us = now_us + c->n_cr_us;
        c->err = ISOTP_ERR_NONE;
        c->len = 0;
        if (act) {
            act->send_fc = true;
            act->fc_id = f->id - ISOTP_FC_ID_OFFSET;
            memset(act->fc_data, 0, sizeof act->fc_data); // 남는 자리는 0x00 (v1과 같다)
            act->fc_data[0] = FC_CTS;
            act->fc_data[1] = FC_BLOCK_SIZE;
            act->fc_data[2] = FC_STMIN;
        }
        return ISOTP_R_IN_PROGRESS;
    }

    if (pci == PCI_CF) {
        if (!c->receiving || f->id != c->rx_id) return ISOTP_R_NOT_MINE; // 받는 중이 아닌 CF는 남의 것
        if ((f->data[0] & 0x0F) != c->next_sn) return fail(c, ISOTP_ERR_WRONG_SN);
        int remain = c->total - c->got;
        int want = remain < CF_DATA_BYTES ? remain : CF_DATA_BYTES;
        if (dlc < want + 1) {
            // 실어야 할 바이트보다 짧은 CF. 여기까지 온 데이터는 믿을 수 있으니 돌려주고 끝낸다.
            // 시간 초과(1초)를 기다리지 않는 것은, 같은 상대가 다음 CF를 제대로 보낼 근거가 없어서다.
            int k = dlc - 1;
            if (k > 0) memcpy(c->buf + c->got, f->data + 1, (size_t)k);
            c->got = (uint16_t)(c->got + (k > 0 ? k : 0));
            return fail(c, ISOTP_ERR_BAD_LEN);
        }
        memcpy(c->buf + c->got, f->data + 1, (size_t)want);
        c->got = (uint16_t)(c->got + want);
        c->next_sn = (uint8_t)((c->next_sn + 1) & 0x0F); // 15 다음 0
        c->deadline_us = now_us + c->n_cr_us;
        if (c->got >= c->total) {
            c->receiving = false;
            c->len = c->total;
            c->err = ISOTP_ERR_NONE;
            return ISOTP_R_COMPLETE;
        }
        return ISOTP_R_IN_PROGRESS;
    }

    // 흐름 제어(3x)나 정의되지 않은 PCI(4~F)는 응답 데이터가 아니다
    (void)PCI_FC;
    return ISOTP_R_NOT_MINE;
}

isotp_result_t isotp_rx_tick(isotp_rx_t *c, int64_t now_us)
{
    if (!c->receiving) return ISOTP_R_NOT_MINE;
    if (now_us >= c->deadline_us) return fail(c, ISOTP_ERR_TIMEOUT_CR);
    return ISOTP_R_IN_PROGRESS;
}

const char *isotp_err_key(isotp_err_t e)
{
    switch (e) {
    case ISOTP_ERR_WRONG_SN: return "wrong_sn";
    case ISOTP_ERR_TIMEOUT_CR: return "timeout_cr";
    case ISOTP_ERR_BAD_LEN: return "bad_len";
    default: return "none";
    }
}

bool isotp_make_sf(const uint8_t *payload, int n, uint8_t out[8])
{
    if (n < 1 || n > SF_MAX_LEN) return false;
    memset(out, 0, 8);
    out[0] = (uint8_t)n;
    memcpy(out + 1, payload, (size_t)n);
    return true;
}
