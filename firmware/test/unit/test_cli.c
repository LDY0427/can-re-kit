// 명령 줄 시험: 골든 파일(test/golden/<이름>.in → <이름>.out)과 해석기 견고성
//   골든은 실제 보드의 시리얼과 같은 내용(프롬프트 "> " 포함)을 가상 버스 위에서 만든 것이다.
//   --update로 돌리면 .out을 다시 쓴다(사람이 내용을 보고 커밋한다).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../port_sim/sim.h"
#include "console_capture.h"
#include "sct/busguard.h"
#include "sct/cli.h"
#include "sct/out.h"
#include "tmini.h"

extern bool g_update_golden;
extern const char *g_golden_dir;

static const char *const k_golden[] = {
    "hello_help",      "ping",          "req_rpm",        "req_multiframe", "dtc",      "poll_three_forms", "scan21_ok",
    "scan21_engine_running", "scan21_dtc_change", "blocked_services", "target_speed", "loopback", "parser_errors",
    "isotp_faults",    "pending78",     "no_ack",         "force",          "error_passive", "watch_jam", "listen_stream",
    "late_response",   "pids",          "line_handling",
};
#define N_GOLDEN (int)(sizeof k_golden / sizeof k_golden[0])

static char *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1); // 시험 프로그램만 heap을 쓴다(펌웨어 핵심은 쓰지 않는다)
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = 0;
    if (n) *n = got;
    return buf;
}

static int g_idx;

static void run_golden(void)
{
    const char *name = k_golden[g_idx];
    char path[256];
    snprintf(path, sizeof path, "%s/%s.in", g_golden_dir, name);
    char *in = read_file(path, NULL);
    CHECK(in != NULL);
    if (!in) return;
    // .in 안의 \x7f 같은 표기는 쓰지 않는다. 줄 끝 CR LF도 그대로 넣는다(줄 처리 시험)
    cap_reset();
    sim_reset();
    sct_out_init(false);
    cli_init();
    cli_banner();
    cap_set_input(in);
    while (cli_step()) {
    }
    snprintf(path, sizeof path, "%s/%s.out", g_golden_dir, name);
    if (g_update_golden) {
        FILE *f = fopen(path, "wb");
        CHECK(f != NULL);
        if (f) {
            fwrite(cap_text(), 1, cap_len(), f);
            fclose(f);
        }
    } else {
        size_t n = 0;
        char *want = read_file(path, &n);
        CHECK(want != NULL);
        if (want) {
            bool same = n == cap_len() && !memcmp(want, cap_text(), n);
            if (!same) { // 처음 다른 줄을 보인다
                size_t i = 0;
                const char *a = want, *b = cap_text();
                while (a[i] && b[i] && a[i] == b[i]) i++;
                size_t ls = i;
                while (ls && a[ls - 1] != '\n') ls--;
                printf("  golden %s differs at byte %zu\n  want: %.120s\n  got:  %.120s\n", name, i, a + ls, b + ls);
            }
            CHECK(same);
            free(want);
        }
    }
    CHECK_EQ_INT(sim_violations(), 0); // 어떤 골든에서도 읽기 밖 요청이 나가지 않는다
    CHECK(!busguard_is_open());
    free(in);
}

static void t_parse_helpers(void)
{
    uint8_t b;
    uint32_t id;
    int v;
    CHECK(cli_parse_hex_byte("0", &b) && b == 0);
    CHECK(cli_parse_hex_byte("fF", &b) && b == 0xFF);
    CHECK(!cli_parse_hex_byte("", &b));
    CHECK(!cli_parse_hex_byte("100", &b));
    CHECK(!cli_parse_hex_byte("g1", &b));
    CHECK(!cli_parse_hex_byte("0x", &b));
    CHECK(!cli_parse_hex_byte("-1", &b));
    CHECK(cli_parse_hex_id("7df", &id) && id == 0x7DF);
    CHECK(!cli_parse_hex_id("1234", &id));
    CHECK(cli_parse_int("500", 0, 1000, &v) && v == 500);
    CHECK(!cli_parse_int("1001", 0, 1000, &v));
    CHECK(!cli_parse_int("-5", -10, 10, &v)); // 부호는 받지 않는다
    CHECK(!cli_parse_int("12a", 0, 1000, &v));
    CHECK(!cli_parse_int("", 0, 1000, &v));
    CHECK(!cli_parse_int("9999999999", 0, 2000000000, &v)); // 넘침 없이 거절
}

static void t_long_line(void)
{
    static char in[2048];
    memset(in, 'a', 1500);
    memcpy(in, "req ", 4);
    strcpy(in + 1500, "\nreq 21 21\n");
    cap_reset();
    sim_reset();
    sct_out_init(false);
    cli_init();
    cap_set_input(in);
    while (cli_step()) {
    }
    CHECK(cap_contains("ERR,12,line_too_long"));
    CHECK(cap_contains("ANS,")); // 다음 줄은 정상으로 돈다
    CHECK_EQ_INT(cap_count("# done\n"), 2);
}

static void t_crlf(void)
{
    // 줄 끝 CR LF(윈도 터미널)는 명령 하나로 친다. CR만 와도 줄이 끝난다
    cap_reset();
    sim_reset();
    sct_out_init(false);
    cli_init();
    cap_set_input("hello\r\nhello\rhello\n");
    while (cli_step()) {
    }
    CHECK_EQ_INT(cap_count("HELLO,"), 3);
    CHECK_EQ_INT(cap_count("# done\n"), 3);
}

static uint32_t rng = 12345;
static uint32_t rnd(void)
{
    rng = rng * 1103515245u + 12345u;
    return rng >> 8;
}

static void t_fuzz(void)
{
    // 무작위 줄: 명령 낱말 + 무작위 인자(16진·숫자·쓰레기·아주 긴 낱말). 죽지 않고 줄마다 "# done"으로 끝나야 한다.
    // 시간이 오래 걸리는 명령(listen·watch·auto·poll·scan21)은 짧은 인자만 쓴다.
    static const char *cmds[] = {"req", "rpm", "target", "speed", "print", "ping", "status", "close", "hello", "dtc", "sim", "loopback", "foo", "REQ",
                                 "poll", "scan21", "listen", "watch", "", "force", "/", "every"};
    static const char *args[] = {"21", "zz", "7DF", "7E0", "500", "250", "0", "-1", "/", "every", "force", "FFFFFFFFFFFF", "on", "off", "bench",
                                 "1", "2", "3", "x", "", "01", "05", "03", "14", "2E", "999999999999999999999", "\t", ","};
    char line[400];
    int done_expected = 0;
    cap_reset();
    sim_reset();
    sct_out_init(false);
    cli_init();
    for (int i = 0; i < 3000; i++) {
        int w = 0;
        int c = (int)(rnd() % (sizeof cmds / sizeof cmds[0]));
        w += snprintf(line + w, sizeof line - (size_t)w, "%s", cmds[c]);
        int na = (int)(rnd() % 12);
        bool slow = !strcmp(cmds[c], "poll") || !strcmp(cmds[c], "scan21") || !strcmp(cmds[c], "listen") || !strcmp(cmds[c], "watch");
        for (int k = 0; k < na && w < (int)sizeof line - 40; k++) {
            const char *a = args[rnd() % (sizeof args / sizeof args[0])];
            if (slow && k < 2 && (!strcmp(a, "500") || !strcmp(a, "250"))) a = "3"; // 긴 실행을 피한다
            w += snprintf(line + w, sizeof line - (size_t)w, " %s", a);
        }
        bool empty = true;
        for (char *p = line; *p; p++)
            if (*p != ' ' && *p != '\t' && *p != ',') empty = false;
        cli_exec(line);
        if (!empty) done_expected++;
        if (cap_len() > (3u << 20)) cap_clear(); // 넘치지 않게 비운다(센 값은 아래에서 따로)
    }
    CHECK_EQ_INT(sim_violations(), 0);
    CHECK(!busguard_is_open());
    (void)done_expected;
}

static void t_fuzz_done_count(void)
{
    // 무작위 바이트 줄(인쇄 가능한 글자만, cli_step으로) → 줄마다 "# done" 하나
    static char in[64 * 1024];
    size_t w = 0;
    int lines = 0;
    rng = 777;
    while (w < sizeof in - 200) {
        int len = 1 + (int)(rnd() % 120);
        bool nonblank = false;
        for (int i = 0; i < len; i++) {
            char ch = (char)(33 + rnd() % 94);
            if (rnd() % 5 == 0) ch = ' ';
            if (ch != ' ' && ch != ',') nonblank = true;
            in[w++] = ch;
        }
        in[w++] = '\n';
        if (nonblank) lines++;
    }
    in[w] = 0;
    cap_reset();
    sim_reset();
    sct_out_init(false);
    cli_init();
    cap_set_input(in);
    while (cli_step()) {
    }
    CHECK_EQ_INT(cap_count("# done\n"), lines);
    CHECK_EQ_INT(sim_violations(), 0);
}

void group_cli(void)
{
    static char names[N_GOLDEN][64];
    for (g_idx = 0; g_idx < N_GOLDEN; g_idx++) {
        snprintf(names[g_idx], sizeof names[0], "cli_golden_%s", k_golden[g_idx]);
        t_run(names[g_idx], run_golden);
    }
    t_run("cli_parse_helpers", t_parse_helpers);
    t_run("cli_long_line", t_long_line);
    t_run("cli_crlf", t_crlf);
    t_run("cli_fuzz_3000_lines", t_fuzz);
    t_run("cli_fuzz_random_bytes_done_per_line", t_fuzz_done_count);
}
