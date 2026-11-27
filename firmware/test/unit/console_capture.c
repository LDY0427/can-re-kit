#include "console_capture.h"

#include <string.h>
#include "sct/out.h"
#include "sct/port.h"

#define CAP_SIZE (4u << 20) // 4 MiB. 넘으면 뒤를 버리고 넘쳤다고 표시한다

static struct {
    char out[CAP_SIZE + 1];
    size_t n;
    bool overflow;
    const char *in;
    size_t in_pos;
    size_t line_end; // 지금 내주는 줄의 끝(\n 다음). 이 앞까지만 읽힌다
    bool autodrain;
    bool key;
} c;

void cap_reset(void)
{
    c.n = 0;
    c.overflow = false;
    c.out[0] = 0;
    c.in = NULL;
    c.in_pos = c.line_end = 0;
    c.autodrain = true;
    c.key = false;
}

void cap_clear(void)
{
    c.n = 0;
    c.out[0] = 0;
}

const char *cap_text(void) { return c.out; }
size_t cap_len(void) { return c.n; }
void cap_set_autodrain(bool on) { c.autodrain = on; }
void cap_set_key(bool pressed) { c.key = pressed; }

void cap_set_input(const char *text)
{
    c.in = text;
    c.in_pos = c.line_end = 0;
}

bool cap_contains(const char *needle) { return strstr(c.out, needle) != NULL; }

int cap_count(const char *needle)
{
    int k = 0;
    size_t nl = strlen(needle);
    for (const char *p = c.out; (p = strstr(p, needle)) != NULL; p += nl) k++;
    return k;
}

void port_con_write(const char *s, size_t n)
{
    if (c.n + n > CAP_SIZE) {
        c.overflow = true;
        n = CAP_SIZE - c.n;
    }
    memcpy(c.out + c.n, s, n);
    c.n += n;
    c.out[c.n] = 0;
}

int port_con_getc(int timeout_ms)
{
    (void)timeout_ms;
    if (!c.in) return SCT_CON_EOF;
    if (c.in_pos >= c.line_end) { // 앞 줄을 다 읽었다 → 다음 줄을 보인다(앞 명령이 끝난 뒤에만 여기로 온다)
        if (!c.in[c.in_pos]) return SCT_CON_EOF;
        const char *nl = strchr(c.in + c.in_pos, '\n');
        c.line_end = nl ? (size_t)(nl - c.in) + 1 : strlen(c.in);
    }
    return (unsigned char)c.in[c.in_pos++];
}

bool port_con_key_pressed(void)
{
    bool k = c.key;
    c.key = false;
    return k;
}

void port_out_kick(void)
{
    if (c.autodrain && sct_out_buffered()) sct_out_drain((size_t)-1);
}
