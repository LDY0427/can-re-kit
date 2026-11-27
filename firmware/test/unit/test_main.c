// 단위 시험 실행 파일(wasm). 사용: node run_tests.mjs tests.wasm [--update] [--golden <폴더>]
#include <stdio.h>
#include <string.h>
#include "tmini.h"

int t_fail_in_test;
const char *t_current;
bool g_update_golden;
const char *g_golden_dir = "/golden";

static int g_total, g_passed;

void t_run(const char *name, t_fn fn)
{
    t_current = name;
    t_fail_in_test = 0;
    fn();
    g_total++;
    if (!t_fail_in_test) g_passed++;
    printf("%s %s\n", t_fail_in_test ? "FAIL" : "ok  ", name);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--update")) g_update_golden = true;
        if (!strcmp(argv[i], "--golden") && i + 1 < argc) g_golden_dir = argv[++i];
    }
    group_isotp();
    group_policy();
    group_busguard();
    group_stats();
    group_dtc();
    group_out();
    group_diag();
    group_cli();
    printf("\n%d tests, %d passed, %d failed%s\n", g_total, g_passed, g_total - g_passed, g_update_golden ? " (golden files updated)" : "");
    return g_passed == g_total ? 0 : 1;
}
