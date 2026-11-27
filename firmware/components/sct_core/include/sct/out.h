// 줄 출력. 모든 출력은 여기를 지난다.
//   버퍼 모드(ESP): 줄을 링 버퍼에 넣고, 낮은 우선순위 태스크가 sct_out_drain()으로 콘솔에 내보낸다.
//     CAN을 받는 태스크가 UART 쓰기에 묶이지 않게 하려는 것이다. 순서는 넣은 순서 그대로다.
//     버퍼가 차면 RX 줄(sct_out_frame의 "RX")만 버리고 수를 센다. 나머지 줄은 자리가 날 때까지 기다린다.
//   바로 모드(PC 가상 빌드·시험): 넣는 즉시 port_con_write로 쓴다.
#ifndef SCT_OUT_H
#define SCT_OUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sct/port.h"

#ifdef __cplusplus
extern "C" {
#endif

// 링 버퍼 크기. 2의 거듭제곱. [가정] 921600 baud에서 약 0.18초 분량. 초당 2,000프레임(v1 한계 문구)이면
// RX 줄 약 40바이트 × 2,000 = 80 kB/s라 콘솔(약 92 kB/s)과 거의 같아 버퍼가 밀리기 시작한다.
#define SCT_OUT_RING_SIZE 16384u
// 한 번에 형식을 만드는 줄의 최대 길이. 이보다 긴 데이터는 sct_out_hex로 나눠 넣는다
#define SCT_OUT_LINE_MAX 512

void sct_out_init(bool buffered);
bool sct_out_buffered(void);
void sct_out_raw(const char *s, size_t n); // 버리지 않는 출력(프롬프트 등)
#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
void sct_outf(const char *fmt, ...);       // 버리지 않는 출력. 줄 끝 \n은 부르는 쪽이 넣는다
// prefix + 16진(공백 구분) + suffix. 길이에 제한이 없다(조각으로 나눠 넣는다). 버리지 않는다
void sct_out_hex(const char *prefix, const uint8_t *d, size_t n, const char *suffix);
// 프레임 한 줄: 종류,시각(ms.us),ID,길이,데이터. tag가 "RX"이면 버퍼가 찼을 때 버릴 수 있다.
// print 설정(print on|off)은 여기서 보지 않는다. 부르는 쪽이 sct_out_print_enabled()로 고른다
void sct_out_frame(const char *tag, const sct_frame_t *f);
void sct_out_set_print(bool on);
bool sct_out_print_enabled(void);

size_t sct_out_drain(size_t max);   // 내보내는 쪽(한 태스크만). 내보낸 바이트 수
size_t sct_out_pending(void);       // 아직 안 나간 바이트 수
void sct_out_flush(void);           // 다 나갈 때까지 기다린다(바로 모드는 즉시 돌아온다)
uint32_t sct_out_take_dropped(void); // 버린 RX 줄 수를 돌려주고 0으로

// 형식 도우미
void sct_fmt_time(char *buf, size_t cap, int64_t t_us);                     // "밀리초.마이크로초 세 자리"
size_t sct_fmt_hex(char *dst, size_t cap, const uint8_t *d, size_t n);      // "01 AB ..." 넘치면 자른다

#ifdef __cplusplus
}
#endif
#endif
