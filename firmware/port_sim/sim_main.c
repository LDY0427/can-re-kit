// 대화형 가상 펌웨어의 시작점. 실제 보드의 app_main과 같은 순서로 돈다(출력은 바로 모드).
#include "sct/cli.h"
#include "sct/out.h"
#include "sim.h"

int main(void)
{
    sim_reset();
    sct_out_init(false); // PC는 출력이 CAN 수신을 막을 일이 없어 바로 쓴다
    cli_init();
    cli_banner();
    while (cli_step()) {
    }
    return 0;
}
