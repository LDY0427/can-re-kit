// 요청 결과 번호와 ERR 줄의 열쇠말. 음수 번호는 v1 diag_request의 돌려주는 값과 같다
// (POLL·LID 줄의 "실패,<번호>"를 PC 프로그램이 그대로 읽을 수 있게). -7은 v2에서 더했다.
#ifndef SCT_CODES_H
#define SCT_CODES_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SCT_OK = 1,                // 응답을 받았다(부분 응답 포함)
    SCT_NO_RESPONSE = 0,       // ACK는 받았는데 응답이 없다
    SCT_NO_ACK = -1,           // 받아 주는 장치가 없다
    SCT_DRIVER_ERR = -2,       // 드라이버 오류
    SCT_BLOCKED = -3,          // 정책이 막은 요청
    SCT_TALK_OFF = -4,         // TALK 스위치 꺼짐(모듈 보드)
    SCT_BUS_ERR_REFUSED = -5,  // 송신 전 듣기에서 버스 오류가 보여 들어가지 않았다
    SCT_BUS_OFF = -6,          // bus-off
    SCT_ERR_PASSIVE = -7,      // 오류 카운터 128 이상(error passive). v2에서 더함
} sct_status_t;

// ERR,<번호>,<열쇠말> 줄의 번호. 위 상태와 별개의 양수 번호다(PC 프로그램이 열쇠말로 고른다)
typedef enum {
    SCT_E_NO_RESPONSE = 1,
    SCT_E_NO_ACK = 2,
    SCT_E_DRIVER = 3,
    SCT_E_BLOCKED = 4,
    SCT_E_TALK_OFF = 5,
    SCT_E_BUS_ERR_REFUSED = 6,
    SCT_E_BUS_OFF = 7,
    SCT_E_ERR_PASSIVE = 8,
    SCT_E_JAM_STOP = 9,
    SCT_E_BAD_ARGS = 10,
    SCT_E_BAD_HEX = 11,
    SCT_E_LINE_TOO_LONG = 12,
    SCT_E_BAD_SPEED = 13,
    SCT_E_BAD_TARGET = 14,
    SCT_E_UNKNOWN_CMD = 15,
    SCT_E_ENGINE_RUNNING = 16,
    SCT_E_RPM_UNKNOWN = 17,
    SCT_E_DTC_CHANGED = 18,
    SCT_E_DTC_UNKNOWN = 19,
    SCT_E_LOOPBACK_REFUSED = 20,
    SCT_E_LOOPBACK_FAIL = 21,
    SCT_E_FORCE_REFUSED = 22,
} sct_err_t;

const char *sct_err_key(sct_err_t e);
sct_err_t sct_err_of_status(int status); // 실패 상태 → ERR 번호. 성공이면 0

#ifdef __cplusplus
}
#endif
#endif
