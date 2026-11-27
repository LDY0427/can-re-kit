// 줄 출력과 링 버퍼. 넣는 쪽(명령 태스크) 하나, 빼는 쪽(출력 태스크) 하나인 단일 생산자·단일 소비자 구조다.
// 두 태스크가 다른 코어에서 돌 수 있어(ESP32-S3 듀얼 코어) 인덱스는 C11 원자 변수로 주고받는다.
// 인덱스는 계속 늘어나는 값이고, 버퍼 위치는 인덱스 & (크기 - 1)이다. size_t가 넘쳐 돌아도 뺄셈은 맞다.
#include "sct/out.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define RING_MASK (SCT_OUT_RING_SIZE - 1u)
_Static_assert((SCT_OUT_RING_SIZE & RING_MASK) == 0, "SCT_OUT_RING_SIZE must be a power of two");

static struct {
    bool buffered;
    bool print_rx;
    atomic_size_t head; // 넣는 쪽만 쓴다
    atomic_size_t tail; // 빼는 쪽만 쓴다
    uint32_t dropped;   // 넣는 쪽만 쓴다
    char ring[SCT_OUT_RING_SIZE];
} s = {.print_rx = true};

void sct_out_init(bool buffered)
{
    s.buffered = buffered;
    s.print_rx = true;
    atomic_store(&s.head, 0);
    atomic_store(&s.tail, 0);
    s.dropped = 0;
}

bool sct_out_buffered(void) { return s.buffered; }
void sct_out_set_print(bool on) { s.print_rx = on; }
bool sct_out_print_enabled(void) { return s.print_rx; }

static size_t ring_free(void)
{
    size_t h = atomic_load_explicit(&s.head, memory_order_relaxed);
    size_t t = atomic_load_explicit(&s.tail, memory_order_acquire);
    return SCT_OUT_RING_SIZE - (h - t);
}

// n바이트를 통째로 넣는다. 자리가 있는지는 부르는 쪽이 먼저 확인했다
static void ring_put(const char *p, size_t n)
{
    size_t h = atomic_load_explicit(&s.head, memory_order_relaxed);
    for (size_t i = 0; i < n; i++) s.ring[(h + i) & RING_MASK] = p[i];
    atomic_store_explicit(&s.head, h + n, memory_order_release); // 데이터를 다 쓴 뒤에 인덱스를 보인다
    port_out_kick();
}

// 버리지 않는 출력: 자리가 날 때까지 출력 태스크를 깨우며 기다린다.
static void put_wait(const char *p, size_t n)
{
    if (!s.buffered) {
        port_con_write(p, n);
        return;
    }
    while (n) {
        size_t chunk = n > SCT_OUT_RING_SIZE / 2 ? SCT_OUT_RING_SIZE / 2 : n; // 링보다 긴 조각은 나눈다
        while (ring_free() < chunk) {
            port_out_kick();
            port_sleep_ms(1);
        }
        ring_put(p, chunk);
        p += chunk;
        n -= chunk;
    }
}

void sct_out_raw(const char *str, size_t n) { put_wait(str, n); }

void sct_outf(const char *fmt, ...)
{
    char buf[SCT_OUT_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof buf) { // 잘렸다(긴 데이터는 sct_out_hex를 쓴다). UTF-8 글자 중간에서 끊지 않고 줄을 닫는다
        n = (int)sizeof buf - 2;
        while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80) n--;
        buf[n++] = '\n';
    }
    put_wait(buf, (size_t)n);
}

size_t sct_fmt_hex(char *dst, size_t cap, const uint8_t *d, size_t n)
{
    static const char hx[] = "0123456789ABCDEF";
    size_t w = 0;
    if (!cap) return 0;
    for (size_t i = 0; i < n; i++) {
        size_t need = i ? 3 : 2;
        if (w + need + 1 > cap) break; // 끝의 0 자리를 남긴다
        if (i) dst[w++] = ' ';
        dst[w++] = hx[d[i] >> 4];
        dst[w++] = hx[d[i] & 0x0F];
    }
    dst[w] = 0;
    return w;
}

void sct_out_hex(const char *prefix, const uint8_t *d, size_t n, const char *suffix)
{
    char buf[3 * 64 + 2];
    if (prefix) put_wait(prefix, strlen(prefix));
    for (size_t off = 0; off < n; off += 64) { // 64바이트씩
        size_t k = n - off > 64 ? 64 : n - off;
        size_t w = 0;
        if (off) buf[w++] = ' ';
        w += sct_fmt_hex(buf + w, sizeof buf - w, d + off, k);
        put_wait(buf, w);
    }
    if (suffix) put_wait(suffix, strlen(suffix));
}

void sct_fmt_time(char *buf, size_t cap, int64_t t_us)
{
    if (t_us < 0) t_us = 0;
    snprintf(buf, cap, "%" PRId64 ".%03d", t_us / 1000, (int)(t_us % 1000));
}

void sct_out_frame(const char *tag, const sct_frame_t *f)
{
    char line[96], tm[32], hex[32] = "";
    sct_fmt_time(tm, sizeof tm, f->t_us);
    int dlc = f->dlc > 8 ? 8 : f->dlc;
    if (!f->rtr) sct_fmt_hex(hex, sizeof hex, f->data, (size_t)dlc);
    // v1 print_frame과 같은 형식: 종류,시각(ms),ID(16진, 29비트는 앞에 x),길이,데이터
    int n = snprintf(line, sizeof line, "%s,%s,%s%" PRIX32 ",%d,%s\n", tag, tm, f->ext ? "x" : "", f->id, f->dlc, hex);
    if (n < 0) return;
    if ((size_t)n >= sizeof line) n = (int)sizeof line - 1;
    if (s.buffered && strcmp(tag, "RX") == 0) {
        if (ring_free() < (size_t)n) { // 받은 프레임 줄만 버린다. 기다리면 CAN 수신이 밀린다
            s.dropped++;
            return;
        }
        ring_put(line, (size_t)n);
        return;
    }
    put_wait(line, (size_t)n);
}

size_t sct_out_drain(size_t max)
{
    size_t total = 0;
    while (total < max) {
        size_t t = atomic_load_explicit(&s.tail, memory_order_relaxed);
        size_t h = atomic_load_explicit(&s.head, memory_order_acquire);
        size_t avail = h - t;
        if (!avail) break;
        size_t pos = t & RING_MASK;
        size_t k = SCT_OUT_RING_SIZE - pos; // 링 끝까지 이어진 부분만 한 번에
        if (k > avail) k = avail;
        if (k > max - total) k = max - total;
        port_con_write(s.ring + pos, k);
        atomic_store_explicit(&s.tail, t + k, memory_order_release);
        total += k;
    }
    return total;
}

size_t sct_out_pending(void)
{
    return atomic_load_explicit(&s.head, memory_order_acquire) - atomic_load_explicit(&s.tail, memory_order_acquire);
}

void sct_out_flush(void)
{
    if (!s.buffered) return;
    while (sct_out_pending()) {
        port_out_kick();
        port_sleep_ms(1);
    }
}

uint32_t sct_out_take_dropped(void)
{
    uint32_t n = s.dropped;
    s.dropped = 0;
    return n;
}
