// 가상 펌웨어의 콘솔: wasm이 불러 쓰는 호스트 함수(import "host")로 입출력을 넘긴다.
// 왜 WASI 표준 입력을 쓰지 않나: node:wasi의 fd_read는 줄 단위로 막힘 없이 읽혔지만(작은 예로 확인),
// "쌓인 입력이 있는가"를 막지 않고 묻는 방법이 없다(port_con_key_pressed가 필요하다).
// 그래서 test/sim/run_sim.mjs가 표준 입력을 작업 스레드와 나눈 공유 메모리에 받아 두고, 이 함수들이 그것을 읽는다.
#include <stddef.h>
#include "sct/port.h"

__attribute__((import_module("host"), import_name("getc"))) int host_getc(int timeout_ms);
__attribute__((import_module("host"), import_name("avail"))) int host_avail(void);
__attribute__((import_module("host"), import_name("drain"))) void host_drain(void);
__attribute__((import_module("host"), import_name("write"))) void host_write(const char *p, int n);

int port_con_getc(int timeout_ms) { return host_getc(timeout_ms); }

bool port_con_key_pressed(void)
{
    if (!host_avail()) return false;
    host_drain(); // v1처럼 쌓인 입력을 비운다
    return true;
}

void port_con_write(const char *s, size_t n) { host_write(s, (int)n); }
void port_out_kick(void) {}
