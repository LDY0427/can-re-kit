// 송신 정책: 허용 서비스·요청 ID·속도를 표 하나로 본다. 기본은 거절이다.
#ifndef SCT_POLICY_H
#define SCT_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCT_CONFIRMED_KBPS 500     // [실측 11-16] ELM327로 이 차의 진단 CAN 속도를 확인했다. force는 이 속도에서만
#define SCT_REQ_ID_FUNCTIONAL 0x7DF // [표준] ISO 15765-4 공용 요청 주소
#define SCT_REQ_ID_PHYS_FIRST 0x7E0 // [표준] ISO 15765-4 ECU 지정 요청 주소 7E0~7E7
#define SCT_REQ_ID_PHYS_LAST 0x7E7
#define SCT_RSP_ID_FIRST 0x7E8      // [표준] 응답 주소 7E8~7EF. 이 차는 7E8 하나만 답했다 [실측]
#define SCT_RSP_ID_LAST 0x7EF
#define SCT_ALLOWED_TEXT "01 02 03 07 09 0A 1A 21 22"

bool policy_service_allowed(uint8_t svc);
// 서비스 이름. 막는 서비스는 왜 막는지 알 수 있게 이름을 붙인다. 모르면 "이름 없는 서비스"
const char *policy_service_name(uint8_t svc);
bool policy_req_id_ok(uint32_t id);
bool policy_speed_ok(int kbps);
bool policy_force_ok(int kbps);
const char *policy_nrc_name(uint8_t nrc);

#ifdef __cplusplus
}
#endif
#endif
