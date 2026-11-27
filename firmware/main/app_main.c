// 순정 ECU 진단 CAN 읽기 도구 v2. ESP32-S3 시작점. 처음 만든 대상은 KTM 390 Duke(2016) 순정 ECU다.
//   명령 해석·프로토콜은 components/sct_core(순수 C11), 하드웨어는 main/port_esp.c.
//   읽기 요청만 보낸다. 쓰기·보안 접근·재시작·고장 코드 지우기 서비스는 sct_core/policy.c가 막는다.
//   2.0.0은 개발보드 + 트랜시버 모듈로 실차에서 썼다. 그 뒤 바뀐 것은 firmware/README.md "확인 수준"을 본다.
#include "port_esp.h"
#include "sct/cli.h"

void app_main(void)
{
    port_esp_init();
    cli_init();
    cli_banner();
    for (;;) cli_step(); // 실제 보드의 콘솔은 닫히지 않는다
}
