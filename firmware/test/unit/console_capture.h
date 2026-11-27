// 시험용 콘솔: 출력을 메모리에 모으고, 입력은 한 줄씩 내준다(앞 명령이 끝나야 다음 줄이 보인다).
#ifndef CONSOLE_CAPTURE_H
#define CONSOLE_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>

void cap_reset(void);
const char *cap_text(void);
size_t cap_len(void);
void cap_clear(void);
void cap_set_input(const char *text);   // 여러 줄. 줄 끝은 \n
void cap_set_autodrain(bool on);        // 버퍼 모드에서 port_out_kick이 바로 다 내보낼지(출력 태스크 흉내)
void cap_set_key(bool pressed);         // 다음 port_con_key_pressed가 true
bool cap_contains(const char *needle);
int cap_count(const char *needle);

#endif
