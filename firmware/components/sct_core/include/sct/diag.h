// 진단 요청-응답. 정책 확인 → 송신 전 듣기(busguard) → 단발 송신 → ISO-TP 재조립 → 요청과 짝짓기.
#ifndef SCT_DIAG_H
#define SCT_DIAG_H

#include <stdbool.h>
#include <stdint.h>
#include "sct/isotp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DIAG_TX_WAIT_MS 300        // v1 tx_once: 송신 완료 알림을 기다리는 시간
#define DIAG_PENDING_EXTEND_MS 2000 // v1 값: 응답 대기(7F xx 78)를 받으면 그때부터 2초. 표준의 확장 대기 시간(P2*)보다 짧게 잡은 [가정]
#define DIAG_HARD_LIMIT_MS 5000     // v1: 요청 시점부터 5초를 넘지 않는다(응답 대기를 몇 번 받아도)
#define DIAG_RECV_SLICE_MS 5        // 수신 대기 한 번의 길이(v1과 같다)
#define DIAG_NRC_PENDING 0x78       // [표준] requestCorrectlyReceived-ResponsePending
#define DIAG_NEG_RSP 0x7F           // [표준] 거부 응답 서비스 번호
#define DIAG_POS_OFFSET 0x40        // [표준] 긍정 응답 = 서비스 + 0x40

typedef struct {
    int status;              // sct_status_t
    uint32_t rsp_id;
    uint16_t len;            // rsp에 복사한 바이트 수
    uint16_t total;          // 응답이 알린 전체 길이
    bool partial;            // len < total (깨졌거나 rsp가 작다)
    isotp_err_t isotp_err;   // 깨진 이유. 잘리기만 했으면 ISOTP_ERR_NONE
    int64_t lat_us;          // 송신 호출 직전 → 응답 마지막 프레임을 꺼낸 시각
    int64_t txdone_us;       // 송신 호출 직전 → 송신 완료 알림을 확인한 시각. 송신하지 않았으면 -1
    int pending78;           // 이 요청에서 받은 응답 대기 횟수
} diag_result_t;

void diag_init(void);
void diag_set_target(uint32_t id);
uint32_t diag_target(void);
void diag_set_speed(int kbps);
int diag_speed(void);
// 요청 하나. 송신이 실패하면(ACK 없음, 드라이버 오류, 버스 오류 거부, bus-off, error passive) 상태를 찍고 드라이버를 내린다.
void diag_request(const uint8_t *req, int n, uint8_t *rsp, int cap, int timeout_ms, diag_result_t *res);
// 응답이 이 요청에 대한 것인가: 서비스 + 0x40과 되돌린 번호, 또는 거부 응답(7F + 서비스)
bool diag_rsp_matches(const uint8_t *req, int n, const uint8_t *d, int len);
int diag_pending_total(void);    // poll 하나 동안 받은 응답 대기 수(poll이 시작할 때 0으로)
void diag_pending_reset(void);

#ifdef __cplusplus
}
#endif
#endif
