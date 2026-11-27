// 포트 인터페이스. sct_core 계층은 이 함수들만으로 돈다. 구현은 링크할 때 정해진다:
//   main/port_esp.c  = ESP32-S3 (TWAI, 콘솔, GPIO, ADC)
//   port_sim/*.c     = PC(WebAssembly) 가상 버스 + 가상 ECU
// 이 헤더는 ESP-IDF·RTOS 헤더를 쓰지 않는다. 표준 C11 헤더만 쓴다.
#ifndef SCT_PORT_H
#define SCT_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// CAN 프레임 하나. t_us = 포트가 프레임을 꺼낸 시각(부팅 뒤 경과, us). 버스 위에서 끝난 시각이 아니다.
typedef struct {
    uint32_t id;
    uint8_t dlc;
    uint8_t data[8];
    bool ext, rtr;
    int64_t t_us;
} sct_frame_t;

typedef enum {
    SCT_MODE_LISTEN,   // 듣기 전용. ACK도 오류 표시도 내지 않는다
    SCT_MODE_NORMAL,   // 일반. ACK를 준다
    SCT_MODE_SELFTEST, // 책상 자체 시험. ACK 없이 송신이 끝나고 자기 프레임을 되받는다(ESP: TWAI_MODE_NO_ACK + self)
} sct_mode_t;

typedef enum {
    SCT_TX_ACKED = 1,
    SCT_TX_NO_ACK = 0,
    SCT_TX_DRIVER_ERR = -1,
    SCT_TX_BUS_OFF = -2,
} sct_tx_result_t;

typedef struct {
    uint32_t bus_errors, rec, tec, tx_failed, rx_missed, arb_lost;
    bool bus_off, err_passive;
} sct_bus_status_t;

// port_con_getc가 돌려주는 값: 0~255 = 받은 바이트, -1 = 시간 안에 없음, SCT_CON_EOF = 입력이 닫혔다(가상 빌드만)
#define SCT_CON_NONE (-1)
#define SCT_CON_EOF (-2)

// LED 번호. 개발보드는 LED가 없어 port_led가 아무것도 안 한다
#define SCT_LED_RX 0
#define SCT_LED_TX 1
#define SCT_LED_ERR 2

bool port_can_open(int kbps, sct_mode_t mode); // 실패하면 false. 이미 열려 있으면 닫고 다시 연다
void port_can_close(void);                     // 드라이버를 내리고 TX 핀을 풀업 입력으로 둔다
// 한 프레임을 한 번만 보낸다(재전송 없음). wait_ms 안에 송신 완료 알림이 없으면 SCT_TX_NO_ACK.
// t_txdone_us(NULL 가능) = 송신 완료 알림을 확인한 시각
sct_tx_result_t port_can_send_once(const sct_frame_t *f, int wait_ms, int64_t *t_txdone_us);
bool port_can_recv(sct_frame_t *f, int timeout_ms); // t_us = 프레임을 꺼낸 시각
bool port_can_status(sct_bus_status_t *s);          // 드라이버가 내려가 있으면 false
int64_t port_now_us(void);
void port_sleep_ms(int ms);
int port_con_getc(int timeout_ms);  // 위 SCT_CON_* 참고
bool port_con_key_pressed(void);    // 쌓인 입력을 비우고, 있었으면 true
void port_con_write(const char *s, size_t n);
bool port_talk_enabled(void);       // 개발보드는 항상 true
void port_led(int which, bool on);  // 개발보드는 아무것도 안 한다
const char *port_board_name(void);

// v2에서 더한 것(명세 2.1에 없음). 보드·빌드마다 다른 명령과 출력 버퍼의 깨우기를 포트로 넘긴다.
// port_board_cmd: volts·selftest(모듈 보드), sim ...(가상 빌드)을 처리했으면 true. argv[0]은 소문자다
bool port_board_cmd(int argc, char **argv);
void port_board_help(void);   // help 끝에 보드 전용 명령 설명을 덧붙인다
void port_board_banner(void); // 부팅 문구 뒤 보드 상태 한 줄
void port_board_status(void); // status 명령의 보드 상태 줄
void port_out_kick(void);     // 출력 링 버퍼에 줄을 넣었다. 내보내는 태스크를 깨운다(바로 쓰는 포트는 빈 함수)

#ifdef __cplusplus
}
#endif
#endif
