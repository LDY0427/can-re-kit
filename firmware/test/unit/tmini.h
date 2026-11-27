// 아주 작은 시험 틀. 시험 함수 하나 = 시험 하나. CHECK가 하나라도 틀리면 그 시험은 실패다.
#ifndef TMINI_H
#define TMINI_H

#include <stdbool.h>
#include <stdio.h>

extern int t_fail_in_test;
extern const char *t_current;

#define CHECK(cond)                                                                                  \
    do {                                                                                             \
        if (!(cond)) {                                                                               \
            t_fail_in_test++;                                                                        \
            printf("  FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, t_current, #cond);                  \
        }                                                                                            \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                                           \
    do {                                                                                             \
        long long _a = (long long)(a), _b = (long long)(b);                                          \
        if (_a != _b) {                                                                              \
            t_fail_in_test++;                                                                        \
            printf("  FAIL %s:%d [%s] %s == %s (%lld != %lld)\n", __FILE__, __LINE__, t_current, #a, #b, _a, _b); \
        }                                                                                            \
    } while (0)

typedef void (*t_fn)(void);
void t_run(const char *name, t_fn fn);

// 시험 그룹(각 test_*.c가 정의한다)
void group_isotp(void);
void group_policy(void);
void group_busguard(void);
void group_stats(void);
void group_dtc(void);
void group_out(void);
void group_diag(void);
void group_cli(void);

#endif
