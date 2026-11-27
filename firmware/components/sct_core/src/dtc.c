// 고장 코드 풀이. [표준] SAE J1979: 코드 하나 = 2바이트. 첫 바이트 위 2비트가 P/C/B/U, 다음 2비트가 첫 숫자,
// 나머지 12비트가 16진 세 자리. CAN(ISO 15765-4)에서는 서비스 바이트 뒤에 개수 바이트가 온다.
//
// 이 ECU [실측 11-17, ELM327 통신 기록 원본]: 개수 바이트는 0000 채움 쌍까지 센 값이라 실린 쌍 수와 같다.
//   03 `43 0C …`(26바이트) = 쌍 12개, 07 `47 05 15 02 00 00 04 80 00 30 00 00` = 쌍 5개(채움 2개).
// 그래도 개수 바이트와 실린 쌍 수가 다른 응답(잘린 응답 등)에 대비해 관대하게 읽는다:
//   개수 × 2가 남은 바이트 안에 들면 개수만큼, 넘으면 실린 쌍 전부를 읽고 어긋났다고 표시한다. 0000 쌍은 건너뛴다.
#include "sct/dtc.h"

#include <string.h>

#define DTC_POSITIVE_OFFSET 0x40 // [표준] 긍정 응답 = 서비스 + 0x40

bool dtc_decode(uint8_t svc, const uint8_t *rsp, int len, dtc_list_t *out)
{
    memset(out, 0, sizeof *out);
    out->svc = svc;
    out->count_byte = -1;
    if (len < 1 || rsp[0] != (uint8_t)(svc + DTC_POSITIVE_OFFSET)) return false;
    const uint8_t *d = rsp + 1;
    int n = len - 1;
    if (n >= 1) { // CAN 형식: 개수 바이트
        out->count_byte = d[0];
        d++;
        n--;
    }
    int pairs = n / 2;
    if (out->count_byte >= 0 && out->count_byte <= pairs) {
        if (out->count_byte != pairs) out->count_mismatch = true;
        pairs = out->count_byte;
    } else if (out->count_byte > pairs) {
        out->count_mismatch = true;
    }
    out->pairs = pairs;
    for (int i = 0; i < pairs && out->n < DTC_MAX; i++) {
        uint16_t c = (uint16_t)(d[2 * i] << 8 | d[2 * i + 1]);
        if (!c) continue; // 0000 = 빈 자리 채움
        out->codes[out->n++] = c;
    }
    return true;
}

void dtc_text(uint16_t code, char *buf)
{
    static const char sys[] = "PCBU";
    static const char hx[] = "0123456789ABCDEF";
    buf[0] = sys[code >> 14];
    buf[1] = (char)('0' + ((code >> 12) & 3));
    buf[2] = hx[(code >> 8) & 0xF];
    buf[3] = hx[(code >> 4) & 0xF];
    buf[4] = hx[code & 0xF];
    buf[5] = 0;
}
