// 줄 명령 해석과 명령 실행. 명령과 줄 형식은 v1과 호환이고 명세 4절의 v2 줄을 더한다.
#ifndef SCT_CLI_H
#define SCT_CLI_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCT_VERSION "2.0.1"
#define SCT_LINE_FORMAT 2   // HELLO 줄의 "줄 형식 판"
#define CLI_LINE_MAX 160    // v1 line[160]. 이보다 긴 줄은 통째로 버린다(v1은 말없이 잘랐다)
#define CLI_MAX_TOKENS 64   // v1은 20이라 "poll ... (요청 6개) force"의 force가 말없이 빠졌다

void cli_init(void);
void cli_banner(void);    // HELLO 줄과 보드 문구
bool cli_step(void);      // 프롬프트를 내고 한 줄을 읽어 실행한다. 입력이 닫히면 false
void cli_exec(char *line); // 한 줄 실행(줄을 고친다). 끝에 드라이버를 내리고 "# done"
const char *cli_build_date(void);

// 해석 도우미(시험에서도 쓴다)
bool cli_parse_hex_byte(const char *t, uint8_t *out); // 16진 1~2자리만
bool cli_parse_hex_id(const char *t, uint32_t *out);  // 16진 1~3자리(11비트)
bool cli_parse_int(const char *t, int lo, int hi, int *out); // 10진, 범위 안

#ifdef __cplusplus
}
#endif
#endif
