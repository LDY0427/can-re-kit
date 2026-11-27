// 통계: ID 표(개수·주기·바이트 변화), 버스 부하, 응답 지연 분포, 송신 완료 시간.
// 계산만 한다. 출력 줄 형식은 cli.c가 맡는다.
#ifndef SCT_STATS_H
#define SCT_STATS_H

#include <stdbool.h>
#include <stdint.h>
#include "sct/port.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STATS_MAX_IDS 96     // v1과 같다. 이 차의 진단 CAN에는 상시 프레임이 없었다 [실측 11-16·17, ELM327 듣기]
#define STATS_LAT_KEEP 2000  // 이 수까지는 표본을 그대로 모아 정렬해서 중앙값·95 %를 정확히 낸다(v1 POLL_KEEP)
// 표본이 STATS_LAT_KEEP보다 많으면 아래 칸 나눔으로 낸다. v1·v2.0.0은 앞 2,000개만으로 계산했다.
// [실측 11-18] 5분(15,000개) 구간에서 화면 값 5.59 ms, 전체로 다시 계산한 값 5.85 ms로 달랐다.
#define STATS_FINE_US 10       // 칸 하나의 폭(us). 분위수의 해상도이고, 줄에 찍는 자릿수(0.01 ms)와 같다
#define STATS_FINE_BINS 2000   // 0~20 ms. 실측 지연은 0.7~11.1 ms였다. 20 ms 이상은 마지막 칸 하나에 모은다
#define STATS_HIST_BINS 7    // 2 ms 미만, 2~5, 5~10, 10~20, 20~50, 50~100, 100 ms 이상 (v1 POLLHIST)

typedef struct {
    uint32_t id;
    bool ext;
    uint32_t n;
    uint8_t dlc;         // 마지막 DLC
    int64_t t_first, t_last;
    double per_min_ms, per_max_ms, per_mean_ms, per_m2; // 주기: Welford 누적
    uint8_t first[8], last[8];
    uint8_t changed;     // 바이트 i가 한 번이라도 바뀌었으면 비트 i
} stats_id_t;

// ---- ID 표와 버스 부하
void stats_reset(void);
void stats_add(const sct_frame_t *f);
uint32_t stats_frames(void);
int stats_nids(void);
const stats_id_t *stats_id_at(int i);
double stats_period_std_ms(const stats_id_t *r); // 표본 표준편차. 주기 표본이 2개 미만이면 0
uint64_t stats_bits(void);                       // 받은 프레임의 추정 비트 수 합
// 프레임 한 개의 비트 수(11비트 ID, 최악 스터핑): 47 + 8·DLC + ⌊(34 + 8·DLC − 1)/4⌋  [계산, 명세 4절]
uint32_t stats_frame_bits(int dlc);
double stats_bus_load_pct(uint64_t bits, int kbps, double sec);

// ---- 응답 지연(poll)
typedef struct {
    uint32_t n;
    int64_t sum, min, max;
    uint32_t hist[STATS_HIST_BINS];
    int32_t keep[STATS_LAT_KEEP];
    uint32_t kept;
    uint16_t fine[STATS_FINE_BINS + 1]; // 10 us 칸. 칸 하나에 65,535개까지(poll 횟수 상한 40,000보다 크다)
} stats_lat_t;

void stats_lat_reset(stats_lat_t *l);
void stats_lat_add(stats_lat_t *l, int64_t us);
int stats_lat_bin(int64_t us);
// 중앙값(k/2번째)과 95 %(k·95/100번째, k-1 넘지 않게)를 낸다. 표본이 없으면 false.
// 표본이 STATS_LAT_KEEP 이하면 정렬해서 정확히, 더 많으면 10 us 칸 나눔에서 낸다(칸의 아래 끝 값, 20 ms 이상이면 최대값).
bool stats_lat_quantiles(stats_lat_t *l, int64_t *median, int64_t *p95);

#ifdef __cplusplus
}
#endif
#endif
