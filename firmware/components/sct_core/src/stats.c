// 통계 계산. heap을 쓰지 않는다(표는 고정 크기). 정렬은 C 표준 라이브러리 qsort.
#include "sct/stats.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_FIXED_BITS 47   // [표준] ISO 11898-1 11비트 데이터 프레임: SOF~IFS의 데이터 밖 비트(1+11+1+1+1+4+15+1+2+7+3)
#define FRAME_STUFF_REGION 34 // [표준] 비트 스터핑이 걸리는 데이터 밖 비트(SOF~CRC: 1+11+1+1+1+4+15)
#define STUFF_EVERY 4         // [계산] 최악의 경우 첫 5비트 뒤로 4비트마다 스터핑 비트 하나 → ⌊(n−1)/4⌋

static const int k_edge_ms[STATS_HIST_BINS - 1] = {2, 5, 10, 20, 50, 100}; // v1 cmd_poll의 경계

static struct {
    stats_id_t ids[STATS_MAX_IDS];
    int nid;
    uint32_t frames;
    uint64_t bits;
} s;

void stats_reset(void)
{
    s.nid = 0;
    s.frames = 0;
    s.bits = 0;
}

uint32_t stats_frame_bits(int dlc)
{
    if (dlc < 0) dlc = 0;
    if (dlc > 8) dlc = 8;
    uint32_t stuffable = (uint32_t)(FRAME_STUFF_REGION + 8 * dlc);
    return (uint32_t)(FRAME_FIXED_BITS + 8 * dlc) + (stuffable - 1) / STUFF_EVERY;
}

double stats_bus_load_pct(uint64_t bits, int kbps, double sec)
{
    if (kbps <= 0 || sec <= 0) return 0;
    return (double)bits * 100.0 / ((double)kbps * 1000.0 * sec);
}

void stats_add(const sct_frame_t *f)
{
    s.frames++;
    s.bits += stats_frame_bits(f->rtr ? 0 : f->dlc);
    int dlc = f->dlc > 8 ? 8 : f->dlc;
    for (int i = 0; i < s.nid; i++) {
        stats_id_t *r = &s.ids[i];
        if (r->id != f->id || r->ext != f->ext) continue;
        double per = (double)(f->t_us - r->t_last) / 1000.0;
        uint32_t k = r->n; // 이 프레임 앞까지의 주기 표본 수 = n - 1 → 이번이 k번째
        if (k == 1) {
            r->per_min_ms = r->per_max_ms = per;
        } else {
            if (per < r->per_min_ms) r->per_min_ms = per;
            if (per > r->per_max_ms) r->per_max_ms = per;
        }
        double d = per - r->per_mean_ms; // Welford: 표본을 모아 두지 않고 평균·분산을 낸다
        r->per_mean_ms += d / (double)k;
        r->per_m2 += d * (per - r->per_mean_ms);
        for (int b = 0; b < dlc; b++)
            if (f->data[b] != r->first[b]) r->changed |= (uint8_t)(1u << b);
        r->n++;
        r->t_last = f->t_us;
        r->dlc = f->dlc;
        memcpy(r->last, f->data, 8);
        return;
    }
    if (s.nid >= STATS_MAX_IDS) return; // 표가 차면 새 ID는 개수에만 들어간다(v1과 같다)
    stats_id_t *r = &s.ids[s.nid++];
    memset(r, 0, sizeof *r);
    r->id = f->id;
    r->ext = f->ext;
    r->n = 1;
    r->dlc = f->dlc;
    r->t_first = r->t_last = f->t_us;
    memcpy(r->first, f->data, 8);
    memcpy(r->last, f->data, 8);
}

uint32_t stats_frames(void) { return s.frames; }
int stats_nids(void) { return s.nid; }
const stats_id_t *stats_id_at(int i) { return (i >= 0 && i < s.nid) ? &s.ids[i] : NULL; }
uint64_t stats_bits(void) { return s.bits; }

double stats_period_std_ms(const stats_id_t *r)
{
    if (r->n < 3) return 0; // 주기 표본 2개 미만
    return sqrt(r->per_m2 / (double)(r->n - 2));
}

void stats_lat_reset(stats_lat_t *l) { memset(l, 0, sizeof *l); }

int stats_lat_bin(int64_t us)
{
    int b = 0;
    while (b < STATS_HIST_BINS - 1 && us >= (int64_t)k_edge_ms[b] * 1000) b++;
    return b;
}

void stats_lat_add(stats_lat_t *l, int64_t us)
{
    if (!l->n || us < l->min) l->min = us;
    if (!l->n || us > l->max) l->max = us;
    l->n++;
    l->sum += us;
    l->hist[stats_lat_bin(us)]++;
    if (l->kept < STATS_LAT_KEEP) l->keep[l->kept++] = us > INT32_MAX ? INT32_MAX : (int32_t)us;
    int64_t b = us < 0 ? 0 : us / STATS_FINE_US;
    if (b > STATS_FINE_BINS) b = STATS_FINE_BINS;
    if (l->fine[b] < UINT16_MAX) l->fine[b]++;
}

static int cmp_i32(const void *a, const void *b)
{
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
    return (x > y) - (x < y);
}

// 칸 나눔에서 rank번째(0부터) 표본이 든 칸의 아래 끝 값. 마지막 칸(20 ms 이상)이면 최대값을 돌려준다
static int64_t fine_at(const stats_lat_t *l, uint32_t rank)
{
    uint32_t seen = 0;
    for (int b = 0; b < STATS_FINE_BINS; b++) {
        seen += l->fine[b];
        if (seen > rank) return (int64_t)b * STATS_FINE_US;
    }
    return l->max;
}

bool stats_lat_quantiles(stats_lat_t *l, int64_t *median, int64_t *p95)
{
    if (l->n > STATS_LAT_KEEP) { // 모아 둔 표본은 앞부분뿐이다. 전체가 들어간 칸 나눔으로 낸다
        uint32_t i95n = (uint32_t)((uint64_t)l->n * 95 / 100);
        if (i95n >= l->n) i95n = l->n - 1;
        *median = fine_at(l, l->n / 2);
        *p95 = fine_at(l, i95n);
        return true;
    }
    uint32_t k = l->kept;
    if (!k) return false;
    qsort(l->keep, k, sizeof l->keep[0], cmp_i32);
    uint32_t i95 = k * 95 / 100;
    if (i95 >= k) i95 = k - 1;
    *median = l->keep[k / 2];
    *p95 = l->keep[i95];
    return true;
}
