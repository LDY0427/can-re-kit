// 버스 보호: 송신 전 듣기, 방해 감지, 오류 상태 감시. 드라이버 상태(열림·모드·속도)를 여기서 쥔다.
// 판정 함수(*_verdict, busguard_state_of)는 포트를 부르지 않아 단위 시험에서 바로 부른다.
#ifndef SCT_BUSGUARD_H
#define SCT_BUSGUARD_H

#include <stdbool.h>
#include <stdint.h>
#include "sct/port.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BG_PRE_LISTEN_MS 300   // v1: 일반 모드 전에 같은 속도로 0.3초 듣는다 [가정: 상시 프레임 주기가 이보다 짧다면 속도 확인이 된다]
#define BG_JAM_ERR_PER_SEC 50  // v1: 일반 모드에서 1초에 프레임 0개, 버스 오류 50건 초과면 내린다
#define BG_ERR_PASSIVE_LIMIT 128 // [표준] ISO 11898-1: TEC 또는 REC가 128 이상이면 error passive

typedef enum { BG_PRE_GO, BG_PRE_GO_FORCED, BG_PRE_REFUSE } bg_pre_t;
typedef enum { BG_STATE_OK, BG_STATE_PASSIVE, BG_STATE_BUSOFF } bg_state_t;

// 판정(순수 함수)
bg_pre_t busguard_pre_tx_verdict(uint32_t bus_errors, int kbps, bool force);
bool busguard_jam_verdict(bool normal, uint32_t frames_delta, uint32_t err_delta); // true = 내린다
bg_state_t busguard_state_of(const sct_bus_status_t *st);
const char *busguard_state_key(bg_state_t s); // "ok" "passive" "busoff"

// 드라이버 상태
void busguard_set_force(bool on); // 명령 끝 force. 명령마다 cli가 다시 정한다
bool busguard_force(void);
bool busguard_open(int kbps, sct_mode_t mode); // 속도가 틀리거나 드라이버가 안 뜨면 # 줄을 찍고 false
// 같은 속도의 일반 모드가 아니면 0.3초 듣고 들어간다. 0 = 일반 모드, SCT_DRIVER_ERR, SCT_BUS_ERR_REFUSED
int busguard_ensure_normal(int kbps);
int busguard_enter_normal(int kbps);
void busguard_close(void);
bool busguard_is_open(void);
bool busguard_is_normal(void);
int busguard_kbps(void);
uint32_t busguard_bus_errors(void);
// 일반 모드에서 오류 상태를 본다. 0 = 괜찮다, SCT_ERR_PASSIVE·SCT_BUS_OFF = 내려야 한다(# 줄은 여기서 찍는다)
int busguard_check(void);
void busguard_print_status(void); // "# 상태:" 줄(v1 형식)과 STAT 줄. 드라이버가 내려가 있으면 아무것도 안 찍는다

#ifdef __cplusplus
}
#endif
#endif
