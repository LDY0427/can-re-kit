// 고장 코드(서비스 03·07·0A) 응답 풀이.
#ifndef SCT_DTC_H
#define SCT_DTC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DTC_MAX 64

typedef struct {
    uint8_t svc;         // 요청 서비스(03·07·0A)
    int count_byte;      // 응답의 개수 바이트(CAN 형식은 서비스 바로 뒤). 없으면 -1
    int pairs;           // 실제로 실린 2바이트 쌍 수
    bool count_mismatch; // 개수 바이트와 실린 쌍 수가 다르다(이 ECU에서는 본 적 없다. 잘린 응답 대비)
    int n;               // 0000 채움을 뺀 코드 수
    uint16_t codes[DTC_MAX];
} dtc_list_t;

// rsp = 응답 전체(첫 바이트 = 서비스 + 0x40). 긍정 응답이 아니면 false
bool dtc_decode(uint8_t svc, const uint8_t *rsp, int len, dtc_list_t *out);
// "P1502" 같은 5글자. buf는 6바이트 이상
void dtc_text(uint16_t code, char *buf);

#ifdef __cplusplus
}
#endif
#endif
