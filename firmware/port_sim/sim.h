// 가상 버스 + 가상 ECU (PC WebAssembly 빌드 전용). sct/port.h의 CAN·시간 함수를 가상 시간으로 구현한다.
// 실제 시간에 기대지 않으므로 같은 명령이면 같은 출력이 나온다(시험이 결정적이다).
#ifndef SCT_SIM_H
#define SCT_SIM_H

#include <stdbool.h>
#include <stdint.h>
#include "sct/port.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SIM_BOOT_US 1000000 // 가상 시계는 부팅 뒤 1초에서 시작한다(시각 줄이 0으로 시작하지 않게)

void sim_reset(void);          // 버스·ECU·고장 주입을 처음 상태로
int64_t sim_time_us(void);
uint32_t sim_violations(void); // 가상 ECU가 받은 읽기 밖 서비스 요청 수(안전 시험용. 늘 0이어야 한다)
uint32_t sim_requests(void);   // 가상 ECU가 받은 요청 수
uint32_t sim_tool_tx(void);    // 도구가 버스로 낸 프레임 수(요청 + 흐름 제어 + 자체 시험)
void sim_set_talk(bool on);
bool sim_cmd(int argc, char **argv); // "sim ..." 명령. argv[0] = "sim"

// ---- 아래는 port_sim 안에서만 쓴다(버스 ↔ ECU)
void ecu_reset(void);
int64_t ecu_next_event(void);                    // 다음 일이 생기는 가상 시각. 없으면 INT64_MAX
bool ecu_pop_event(int64_t t, sct_frame_t *out); // t에 끝나는 ECU 송신 프레임. 내부 일(시간 초과)만이면 false
void ecu_tx_result(bool acked, int64_t t);       // 방금 낸 프레임이 ACK를 받았는가
void ecu_on_frame(const sct_frame_t *f, int64_t t); // ECU가 버스에서 받은 프레임(도구의 요청·흐름 제어)
bool ecu_acks(int kbps);                         // 이 속도의 프레임에 ECU가 ACK를 주는가
int ecu_kbps(void);
bool ecu_other_acker(void);                      // 버스에 ACK를 주는 다른 장치가 있다(sim stream ... ack)
bool ecu_cmd(int argc, char **argv);             // argv[0] = 하위 명령
void ecu_status_line(void);
uint32_t ecu_violations(void);
uint32_t ecu_requests(void);
int64_t sim_frame_us(int dlc, int kbps);         // 프레임 한 개가 버스를 차지하는 시간(최악 스터핑)

#ifdef __cplusplus
}
#endif
#endif
