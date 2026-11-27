// ISO 15765-2 수신 쪽(응답 재조립) 상태 기계. 포트 함수를 부르지 않는다.
// 입력(프레임, 시각)만으로 결정되므로 같은 입력이면 ESP32와 PC에서 같은 결과가 나온다.
// 재진입 가능: 상태는 모두 isotp_rx_t 안에 있다.
#ifndef SCT_ISOTP_H
#define SCT_ISOTP_H

#include <stdbool.h>
#include <stdint.h>
#include "sct/port.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ISOTP_MAX_LEN 4095        // [표준] 12비트 길이 형식의 최대. 32비트 길이 형식(FF 길이 0)은 다루지 않는다
#define ISOTP_N_CR_US 1000000     // [표준] N_Cr 1,000 ms: 다음 이어지는 프레임(CF)을 기다리는 시간
#define ISOTP_FC_ID_OFFSET 8      // [표준] ISO 15765-4: 흐름 제어는 응답 ID - 8 (7E8 → 7E0)

typedef enum {
    ISOTP_ERR_NONE = 0,
    ISOTP_ERR_WRONG_SN,   // 이어지는 프레임 순번이 틀렸다
    ISOTP_ERR_TIMEOUT_CR, // N_Cr 안에 다음 이어지는 프레임이 오지 않았다
    ISOTP_ERR_BAD_LEN,    // 이어지는 프레임의 DLC가 실어야 할 바이트보다 짧다
} isotp_err_t;

typedef enum {
    ISOTP_R_NOT_MINE,    // 진단 응답이 아니거나(ID·형식), 받는 중인 것과 다른 ID, 거른 응답. 부르는 쪽이 RX 줄로 낸다
    ISOTP_R_DISCARD,     // 응답 ID의 프레임이지만 형식이 틀려 버렸다(SF 길이, FF 길이·DLC). 부르는 쪽이 RX 줄로 낸다
    ISOTP_R_IN_PROGRESS, // 여러 프레임을 받는 중
    ISOTP_R_COMPLETE,    // 응답 하나가 끝났다. buf[0..len)
    ISOTP_R_ERROR,       // 받던 응답이 깨졌다. buf[0..len)은 받은 만큼. err에 이유
} isotp_result_t;

typedef struct {
    bool send_fc;     // true면 부르는 쪽이 fc_id로 fc_data(8바이트)를 한 번 보낸다
    uint32_t fc_id;
    uint8_t fc_data[8];
} isotp_action_t;

// 첫 프레임(SF·FF)의 앞 바이트를 보고 이 요청에 대한 응답인지 정한다. NULL이면 다 받는다.
// n = 볼 수 있는 바이트 수(SF는 길이 L, FF는 6)
typedef bool (*isotp_accept_fn)(void *user, const uint8_t *first, int n);

typedef struct {
    // 설정
    uint32_t id_lo, id_hi; // 응답으로 볼 ID 범위(11비트)
    int64_t n_cr_us;
    isotp_accept_fn accept;
    void *accept_user;
    // 상태
    bool receiving;
    uint32_t rx_id;
    uint16_t total, got;
    uint8_t next_sn;
    int64_t deadline_us;
    // 결과
    isotp_err_t err;
    uint16_t len;          // 끝난 응답(또는 깨진 응답)에서 쓸 수 있는 바이트 수
    uint32_t restarts;     // 받는 중에 같은 ID의 새 SF·FF로 다시 시작한 횟수
    uint8_t buf[ISOTP_MAX_LEN];
} isotp_rx_t;

void isotp_rx_init(isotp_rx_t *c, uint32_t id_lo, uint32_t id_hi, isotp_accept_fn accept, void *user);
void isotp_rx_reset(isotp_rx_t *c); // 받던 것을 버린다(설정은 둔다)
isotp_result_t isotp_rx_feed(isotp_rx_t *c, const sct_frame_t *f, int64_t now_us, isotp_action_t *act);
// 시간만 흐를 때. 받는 중 N_Cr이 지나면 ISOTP_R_ERROR(TIMEOUT_CR), 아니면 받는 중이면 IN_PROGRESS, 아니면 NOT_MINE
isotp_result_t isotp_rx_tick(isotp_rx_t *c, int64_t now_us);
const char *isotp_err_key(isotp_err_t e); // "wrong_sn" "timeout_cr" "bad_len" "none"

// 요청 SF 만들기: 8바이트, 남는 자리는 0x00. n은 1~7. 아니면 false
bool isotp_make_sf(const uint8_t *payload, int n, uint8_t out[8]);

#ifdef __cplusplus
}
#endif
#endif
