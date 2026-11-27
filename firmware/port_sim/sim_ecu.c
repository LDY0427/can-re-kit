// 가상 ECU: KTM 390 Duke(2016) 순정 ECU의 진단 응답을 흉내 낸다.
// 응답 값의 근거:
//   [실측 11-17] 21 xx 값 = 사례 차량(case-studies/ktm-390-duke-2016.md) 키 ON·엔진 정지, 스로틀 닫힘 상태의 전체 훑기
//   [실측 11-17] 01 xx 값 = 같은 날 엔진 정지 상태의 표준 항목 읽기
//   [실측 11-16] 01 지원 목록 = 01 05 06 07 0B 0D 0E 0F 11 13 14 1C 21 30 31 4F 50 (01 0C는 답 없음),
//                01 00 응답 = 06 41 00 8E 2E B0 11 AA (ELM327 통신 기록 원본)
//   [실측 11-17] 03·07 응답 프레임, 0A 응답 없음, 패딩 AA = ELM327 통신 기록 원본
//   [가정] 위에 없는 값(01 01 07 13 1C 21 30 31 4F 50의 값, 09, 1A, 22의 답, 응답 지연)
//   [지어낸 값] 09(차량 정보): 차대번호 자리에 실제 번호를 넣지 않는다
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sct/cli.h"
#include "sct/out.h"
#include "sct/policy.h"
#include "sct/stats.h"
#include "sim.h"

#define ECU_RSP_ID 0x7E8              // [실측 11-16] 응답 ID
#define ECU_PHYS_ID 0x7E0             // [표준] 이 ECU의 지정 요청 주소(7E8 - 8)
#define ECU_DELAY_US_DEFAULT 8000     // [가정] 요청 끝 → 응답 시작 8 ms. 직결로 잰 적이 없다(ELM327은 어댑터 지연이 섞였다)
#define ECU_PENDING_GAP_US 500000     // [가정] 응답 대기(7F xx 78)를 다시 보내는 간격
#define ECU_N_BS_US 1000000           // [표준] N_Bs 1,000 ms: FF 뒤 흐름 제어를 기다리는 시간
#define ECU_RETRY_WINDOW_US 50000     // [가정] ACK를 못 받은 프레임을 다시 보내다 포기하는 시간
#define ECU_ERRFRAME_BITS 23          // [표준] 오류 프레임(능동 오류 표시 6 + 겹침 최대 6 + 구분자 8) + 프레임 사이 3
#define ECU_PAD 0xAA                  // [실측] 응답 프레임의 남는 자리(SF `06 41 00 8E 2E B0 11 AA`, 마지막 CF `22 00 AA …`)
#define ECU_BUF 256

typedef enum { PH_NONE, PH_STALE, PH_PEND, PH_FIRST, PH_WAIT_FC, PH_CF } phase_t;
typedef enum { SRC_NONE, SRC_JOB, SRC_STREAM } src_t;

// [실측 11-17] 키 ON·엔진 정지, 21 xx 응답의 데이터(61 xx 뒤)
typedef struct {
    uint8_t lid;
    uint8_t n;
    uint8_t d[5];
} lid_row_t;
static const lid_row_t k_lid[] = {
    {0x20, 4, {0x00, 0x00, 0x23, 0xAA}}, {0x21, 2, {0x00, 0x00}}, {0x22, 1, {0x67}}, {0x23, 1, {0x57}},
    {0x24, 2, {0x00, 0x00}}, {0x25, 1, {0x00}}, {0x26, 1, {0x7D}}, {0x27, 2, {0x00, 0x00}},
    {0x28, 1, {0x00}}, {0x29, 1, {0x5B}}, {0x2A, 1, {0x08}}, {0x2B, 1, {0x85}},
    {0x2C, 2, {0x09, 0x4F}}, {0x2D, 2, {0x00, 0x00}}, {0x2E, 2, {0x80, 0x00}}, {0x2F, 2, {0x80, 0x00}},
    {0x30, 1, {0x00}}, {0x33, 1, {0x00}}, {0x39, 2, {0x03, 0xCD}}, {0x3A, 1, {0x91}},
    {0x3B, 2, {0x02, 0xA8}}, {0x3C, 1, {0x02}}, {0x3D, 1, {0xAA}}, {0x3F, 2, {0x00, 0x81}},
    {0x42, 1, {0x00}}, {0x44, 2, {0x80, 0x00}}, {0x45, 2, {0x00, 0x00}}, {0x48, 1, {0x00}},
    {0x4A, 2, {0x75, 0x9D}}, {0x4B, 5, {0, 0, 0, 0, 0}}, {0x4C, 5, {0, 0, 0, 0, 0}}, {0x61, 1, {0x01}},
    {0x62, 4, {0x00, 0x00, 0x33, 0x3B}}, {0x63, 4, {0, 0, 0, 0}}, {0x64, 4, {0, 0, 0, 0}}, {0x65, 2, {0x01, 0x99}},
    {0x66, 2, {0x03, 0x01}},
};

// 01 xx 값. [실측 11-17] 05 06 0B 0D 0E 0F 11 14, [가정] 나머지(지원 목록에는 있으나 값 기록이 없다)
typedef struct {
    uint8_t pid;
    uint8_t n;
    uint8_t d[4];
} pid_row_t;
static const pid_row_t k_pid[] = {
    {0x01, 4, {0x00, 0x00, 0x00, 0x00}}, {0x05, 1, {0x54}}, {0x06, 1, {0x80}}, {0x07, 1, {0x80}}, {0x0B, 1, {0x75}}, {0x0D, 1, {0x00}},
    {0x0E, 1, {0x80}}, {0x0F, 1, {0x3A}}, {0x11, 1, {0x33}}, {0x13, 1, {0x01}}, {0x14, 2, {0x5C, 0x80}},
    {0x1C, 1, {0x01}}, {0x21, 2, {0x00, 0x00}}, {0x30, 1, {0x00}}, {0x31, 2, {0x00, 0x00}},
    {0x4F, 4, {0x00, 0x00, 0x00, 0x00}}, {0x50, 4, {0x00, 0x00, 0x00, 0x00}},
};

// [실측 11-17] 03 응답 26바이트(저장 코드 12개). 프레임: 10 1A 43 0C 06 50 15 06 / 21 15 02 15 05 16 06 01 /
//   22 18 16 01 04 80 00 30 / 23 01 13 16 07 01 17 AA
// [실측 11-17] 07 응답 12바이트(이번 주행 코드 3개): 10 0C 47 05 15 02 00 00 / 21 04 80 00 30 00 00 AA.
//   개수 바이트 05는 0000 채움 쌍까지 센 값이라 실린 쌍 수(5)와 같다.
//   P1505가 더해진 뒤는 14바이트: 47 06 15 02 00 00 15 05 04 80 00 30 00 00 (새 코드가 셋째 쌍 자리에 들어갔다)
static const uint8_t k_rsp03[] = {0x43, 0x0C, 0x06, 0x50, 0x15, 0x06, 0x15, 0x02, 0x15, 0x05, 0x16, 0x06, 0x01,
                                  0x18, 0x16, 0x01, 0x04, 0x80, 0x00, 0x30, 0x01, 0x13, 0x16, 0x07, 0x01, 0x17};
static const uint8_t k_rsp07[] = {0x47, 0x05, 0x15, 0x02, 0x00, 0x00, 0x04, 0x80, 0x00, 0x30, 0x00, 0x00};
#define RSP07_INSERT_AT 6 // 더한 코드를 넣는 바이트 자리(셋째 쌍). [실측] P1505가 들어간 자리
// [지어낸 값] 09 응답 글자. 실제 차대번호 형식과 섞이지 않게 SIMULATED를 넣었다
static const char k_vin[] = "SIMULATED0VIN0001"; // 17자
static const char k_calid[] = "SIMCAL-0001";
static const char k_ecuname[] = "SIM-ECU";

static struct {
    // 설정·고장 주입
    int kbps;
    bool noack;
    double rpm;
    double revs;      // 크랭크 회전 수(21 64)
    int64_t t_revs;
    int64_t delay_us;
    int pending_n;
    int64_t pending_gap;
    int dropcf, cutcf, badsn, shortcf; // 다음 여러 프레임 응답 하나에만
    bool stale_on;
    uint8_t stale[7];
    int stale_n;
    uint16_t added[8];
    int nadded;
    uint16_t add_code;
    int add_after; // 21 요청을 이 수만큼 더 받으면 add_code를 더한다(0 = 없음)
    uint32_t violations, requests;
    // 상시 프레임
    bool stream_on, stream_ack;
    uint32_t stream_id;
    int64_t stream_period, stream_next;
    uint8_t stream_ctr;
    // 응답 작업
    struct {
        bool active;
        phase_t ph;
        uint8_t buf[ECU_BUF];
        int len, off, cf_idx, pend_left;
        uint8_t sn, svc;
        int64_t next_t, fc_deadline;
        int dropcf, cutcf, badsn, shortcf;
    } job;
    // 버스로 나가는 중인 프레임(ACK를 못 받으면 다시 보낸다)
    struct {
        bool active;
        src_t src;
        sct_frame_t f;
        int64_t t, first_t;
    } out;
} e;

int64_t sim_frame_us(int dlc, int kbps)
{
    return (int64_t)stats_frame_bits(dlc) * 1000 / (kbps > 0 ? kbps : 500);
}

void ecu_reset(void)
{
    memset(&e, 0, sizeof e);
    e.kbps = SCT_CONFIRMED_KBPS;
    e.delay_us = ECU_DELAY_US_DEFAULT;
    e.pending_gap = ECU_PENDING_GAP_US;
}

int ecu_kbps(void) { return e.kbps; }
bool ecu_acks(int kbps) { return !e.noack && kbps == e.kbps; }
bool ecu_other_acker(void) { return e.stream_on && e.stream_ack; }
uint32_t ecu_violations(void) { return e.violations; }
uint32_t ecu_requests(void) { return e.requests; }

static void update_revs(int64_t t)
{
    if (t > e.t_revs) e.revs += e.rpm / 60.0 * (double)(t - e.t_revs) / 1e6;
    e.t_revs = t;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

// PID 묶음 지원 표: base+1 ~ base+32. 다음 묶음이 있으면 base+32 비트도 세운다
static void pid_bitmap(uint8_t base, uint8_t *out4)
{
    uint32_t bits = 0;
    for (size_t i = 0; i < sizeof k_pid / sizeof k_pid[0]; i++) {
        int p = k_pid[i].pid;
        if (p > base && p <= base + 32) bits |= 1u << (32 - (p - base));
        if (p > base + 32) bits |= 1u; // 다음 묶음이 있다
    }
    put_u32(out4, bits);
}

static int text_rsp(uint8_t *out, uint8_t pid, const char *txt, int width)
{
    int n = 0;
    out[n++] = 0x49;
    out[n++] = pid;
    out[n++] = 0x01; // 항목 수
    int len = (int)strlen(txt);
    for (int i = 0; i < width; i++) out[n++] = (uint8_t)(i < len ? txt[i] : 0x00);
    return n;
}

// 고장 코드 응답. 더한 코드(sim adddtc)는 at 바이트 자리에 끼우고 개수 바이트를 그만큼 늘린다
static int dtc_rsp(uint8_t *out, const uint8_t *base, int blen, int at)
{
    memcpy(out, base, (size_t)at);
    int n = at, added = 0;
    for (int i = 0; i < e.nadded; i++) {
        bool dup = false; // 이미 있는 코드는 다시 넣지 않는다(P1505는 저장 코드 12개에 처음부터 있었다 [실측])
        for (int k = 2; k + 1 < blen; k += 2)
            if (((base[k] << 8) | base[k + 1]) == e.added[i]) dup = true;
        if (dup) continue;
        out[n++] = (uint8_t)(e.added[i] >> 8);
        out[n++] = (uint8_t)e.added[i];
        added++;
    }
    memcpy(out + n, base + at, (size_t)(blen - at));
    n += blen - at;
    out[1] = (uint8_t)(out[1] + added);
    return n;
}

// 응답 만들기. 0 = 답하지 않음
static int build_response(uint32_t req_id, const uint8_t *q, int n, uint8_t *out, int64_t t)
{
    bool phys = req_id == ECU_PHYS_ID;
    uint8_t svc = q[0];
    if (!policy_service_allowed(svc)) { // 이 도구는 이런 요청을 보내면 안 된다. 센다
        e.violations++;
        out[0] = 0x7F;
        out[1] = svc;
        out[2] = 0x11; // [가정] 지원하지 않는 서비스
        return 3;
    }
    switch (svc) {
    case 0x01: {
        if (n < 2) return 0;
        uint8_t pid = q[1];
        out[0] = 0x41;
        out[1] = pid;
        if (pid == 0x00 || pid == 0x20 || pid == 0x40) {
            pid_bitmap(pid, out + 2);
            return 6;
        }
        for (size_t i = 0; i < sizeof k_pid / sizeof k_pid[0]; i++)
            if (k_pid[i].pid == pid) {
                memcpy(out + 2, k_pid[i].d, k_pid[i].n);
                return 2 + k_pid[i].n;
            }
        return 0; // [표준] J1979: 지원하지 않는 PID에는 답하지 않는다. 01 0C도 답하지 않았다 [실측 11-16]
    }
    case 0x03: return dtc_rsp(out, k_rsp03, (int)sizeof k_rsp03, (int)sizeof k_rsp03); // 저장 코드는 뒤에 붙인다 [가정]
    case 0x07: return dtc_rsp(out, k_rsp07, (int)sizeof k_rsp07, RSP07_INSERT_AT);
    case 0x0A: return 0; // [실측 11-17] 0A(영구 고장 코드)에는 답하지 않았다(NO DATA). 7E0으로 물을 때도 답하지 않는다고 본다 [가정]
    case 0x09: {
        if (n < 2) return 0;
        switch (q[1]) {
        case 0x00:
            out[0] = 0x49;
            out[1] = 0x00;
            put_u32(out + 2, (1u << (32 - 0x02)) | (1u << (32 - 0x04)) | (1u << (32 - 0x0A)));
            return 6;
        case 0x02: return text_rsp(out, 0x02, k_vin, 17);
        case 0x04: return text_rsp(out, 0x04, k_calid, 16);
        case 0x0A: return text_rsp(out, 0x0A, k_ecuname, 20);
        default: return 0;
        }
    }
    case 0x21: {
        if (n < 2) break;
        uint8_t lid = q[1];
        if (e.add_after > 0 && --e.add_after == 0 && e.nadded < 8) e.added[e.nadded++] = e.add_code; // 고장 코드 하나 생김
        update_revs(t);
        out[0] = 0x61;
        out[1] = lid;
        if (lid == 0x21) { // [실측 11-17] 회전수 = (A×256+B)÷4
            uint32_t v = (uint32_t)(e.rpm * 4.0 + 0.5);
            if (v > 0xFFFF) v = 0xFFFF;
            out[2] = (uint8_t)(v >> 8);
            out[3] = (uint8_t)v;
            return 4;
        }
        if (lid == 0x64 || lid == 0x63) { // [실측 11-17] 21 64 = 크랭크 회전 수, 21 63 = 그 절반(연소 횟수로 추정)
            uint32_t v = (uint32_t)(lid == 0x64 ? e.revs : e.revs / 2.0);
            put_u32(out + 2, v);
            return 6;
        }
        if (lid == 0x42) { // [실측 11-17] 엔진 가동 표시: 정지 00, 회전 01
            out[2] = e.rpm > 0 ? 0x01 : 0x00;
            return 3;
        }
        for (size_t i = 0; i < sizeof k_lid / sizeof k_lid[0]; i++)
            if (k_lid[i].lid == lid) {
                memcpy(out + 2, k_lid[i].d, k_lid[i].n);
                return 2 + k_lid[i].n;
            }
        break;
    }
    default: break; // 02 1A 22: 값을 모른다
    }
    // [가정] 지정 주소(7E0)로 모르는 번호를 물으면 7F xx 12, 공용 주소(7DF)로 물으면 답하지 않는다
    if (!phys) return 0;
    out[0] = 0x7F;
    out[1] = svc;
    out[2] = 0x12;
    return 3;
}

static void start_job(const uint8_t *rsp, int len, uint8_t svc, int64_t t)
{
    memset(&e.job, 0, sizeof e.job);
    e.job.active = true;
    memcpy(e.job.buf, rsp, (size_t)len);
    e.job.len = len;
    e.job.svc = svc;
    e.job.pend_left = e.pending_n;
    if (len > 7) { // 여러 프레임 응답: 고장 주입을 이 응답 하나에 건다
        e.job.dropcf = e.dropcf;
        e.job.cutcf = e.cutcf;
        e.job.badsn = e.badsn;
        e.job.shortcf = e.shortcf;
        e.dropcf = e.cutcf = e.badsn = e.shortcf = 0;
    }
    if (e.stale_on) {
        e.job.ph = PH_STALE;
        e.job.next_t = t + e.delay_us / 2 + sim_frame_us(8, e.kbps);
    } else {
        e.job.ph = e.job.pend_left ? PH_PEND : PH_FIRST;
        e.job.next_t = t + e.delay_us + sim_frame_us(8, e.kbps);
    }
}

void ecu_on_frame(const sct_frame_t *f, int64_t t)
{
    if (e.noack || f->ext || f->rtr || f->dlc < 1) return;
    uint8_t pci = f->data[0] >> 4;
    if (f->id == ECU_PHYS_ID && pci == 0x3 && e.job.active && e.job.ph == PH_WAIT_FC) {
        if ((f->data[0] & 0x0F) == 0) { // 계속 보내라. 블록 크기·간격은 0이라고 보고 붙여 보낸다
            e.job.ph = PH_CF;
            e.job.next_t = t + sim_frame_us(8, e.kbps);
        }
        return;
    }
    if (f->id != SCT_REQ_ID_FUNCTIONAL && f->id != ECU_PHYS_ID) return;
    if (pci != 0) return; // 요청은 SF만 받는다
    int n = f->data[0] & 0x0F;
    if (n < 1 || n > 7 || f->dlc < n + 1) return;
    e.requests++;
    uint8_t rsp[ECU_BUF];
    int len = build_response(f->id, f->data + 1, n, rsp, t);
    if (len <= 0) return;
    start_job(rsp, len, f->data[1], t); // 새 요청이 오면 앞 작업은 버린다 [가정]
}

static void make_sf(sct_frame_t *f, const uint8_t *d, int n)
{
    memset(f, 0, sizeof *f);
    f->id = ECU_RSP_ID;
    f->dlc = 8;
    memset(f->data, ECU_PAD, 8);
    f->data[0] = (uint8_t)n;
    memcpy(f->data + 1, d, (size_t)n);
}

// 응답 작업의 다음 프레임을 만든다. false = 보낼 것이 없다(작업 끝)
static bool job_frame(sct_frame_t *f)
{
    switch (e.job.ph) {
    case PH_STALE:
        make_sf(f, e.stale, e.stale_n);
        return true;
    case PH_PEND: {
        uint8_t d[3] = {0x7F, e.job.svc, 0x78};
        make_sf(f, d, 3);
        return true;
    }
    case PH_FIRST:
        if (e.job.len <= 7) {
            make_sf(f, e.job.buf, e.job.len);
            return true;
        }
        memset(f, 0, sizeof *f);
        f->id = ECU_RSP_ID;
        f->dlc = 8;
        f->data[0] = (uint8_t)(0x10 | (e.job.len >> 8));
        f->data[1] = (uint8_t)e.job.len;
        memcpy(f->data + 2, e.job.buf, 6);
        return true;
    case PH_CF:
        for (;;) {
            if (e.job.off >= e.job.len) return false;
            int k = ++e.job.cf_idx;
            int want = e.job.len - e.job.off < 7 ? e.job.len - e.job.off : 7;
            uint8_t sn = e.job.sn;
            e.job.sn = (uint8_t)((e.job.sn + 1) & 0x0F);
            if (k == e.job.cutcf) return false; // 여기서 끊는다
            if (k == e.job.dropcf) {            // 이 CF는 버스에 나가지 않았다(사라짐)
                e.job.off += want;
                continue;
            }
            memset(f, 0, sizeof *f);
            f->id = ECU_RSP_ID;
            f->dlc = 8;
            memset(f->data, ECU_PAD, 8);
            f->data[0] = (uint8_t)(0x20 | (k == e.job.badsn ? (sn + 1) & 0x0F : sn));
            memcpy(f->data + 1, e.job.buf + e.job.off, (size_t)want);
            if (k == e.job.shortcf) f->dlc = (uint8_t)want; // 한 바이트 모자란 DLC
            e.job.off += want;
            return true;
        }
    default: return false;
    }
}

// 응답 작업의 프레임이 ACK를 받은 뒤 다음 단계로
static void job_advance(int64_t t)
{
    int64_t fr = sim_frame_us(8, e.kbps);
    switch (e.job.ph) {
    case PH_STALE:
        e.stale_on = false;
        e.job.ph = e.job.pend_left ? PH_PEND : PH_FIRST;
        e.job.next_t = t + e.delay_us / 2 + fr;
        break;
    case PH_PEND:
        if (--e.job.pend_left <= 0) e.job.ph = PH_FIRST;
        e.job.next_t = t + e.pending_gap;
        break;
    case PH_FIRST:
        if (e.job.len <= 7) e.job.active = false;
        else {
            e.job.off = 6;
            e.job.sn = 1;
            e.job.ph = PH_WAIT_FC;
            e.job.fc_deadline = t + ECU_N_BS_US;
        }
        break;
    case PH_CF:
        if (e.job.off >= e.job.len) e.job.active = false;
        else e.job.next_t = t + fr;
        break;
    default: break;
    }
}

int64_t ecu_next_event(void)
{
    if (e.out.active) return e.out.t;
    int64_t best = INT64_MAX;
    if (e.job.active) {
        int64_t tj = e.job.ph == PH_WAIT_FC ? e.job.fc_deadline : e.job.next_t;
        if (tj < best) best = tj;
    }
    if (e.stream_on && e.stream_next < best) best = e.stream_next;
    return best;
}

bool ecu_pop_event(int64_t t, sct_frame_t *out)
{
    if (e.out.active) {
        if (e.out.t != t) return false;
        *out = e.out.f;
        return true;
    }
    if (e.job.active && e.job.ph == PH_WAIT_FC && e.job.fc_deadline <= t) { // 흐름 제어가 안 왔다: 응답을 접는다
        e.job.active = false;
        return false;
    }
    if (e.job.active && e.job.ph != PH_WAIT_FC && e.job.next_t <= t) {
        if (!job_frame(&e.out.f)) {
            e.job.active = false;
            return false;
        }
        e.out.src = SRC_JOB;
    } else if (e.stream_on && e.stream_next <= t) {
        sct_frame_t *f = &e.out.f;
        memset(f, 0, sizeof *f);
        f->id = e.stream_id;
        f->dlc = 8;
        f->data[0] = e.stream_ctr++;
        f->data[1] = 0x12; // [지어낸 값] 상시 프레임의 내용은 의미가 없다. 첫 바이트만 계수기로 바뀐다
        f->data[2] = 0x34;
        e.out.src = SRC_STREAM;
    } else return false;
    e.out.active = true;
    e.out.t = e.out.first_t = t;
    *out = e.out.f;
    return true;
}

void ecu_tx_result(bool acked, int64_t t)
{
    if (!e.out.active) return;
    if (!acked && t - e.out.first_t < ECU_RETRY_WINDOW_US) { // ACK 오류 → 오류 프레임 뒤 자동 재전송
        e.out.t = t + (int64_t)ECU_ERRFRAME_BITS * 1000 / e.kbps + sim_frame_us(e.out.f.dlc, e.kbps);
        return;
    }
    src_t src = e.out.src;
    e.out.active = false;
    if (src == SRC_STREAM) {
        e.stream_next += e.stream_period;
        if (e.stream_next <= t) e.stream_next = t + e.stream_period;
    } else if (src == SRC_JOB) {
        if (acked) job_advance(t);
        else e.job.active = false; // 포기
    }
}

// ---------------------------------------------------------------- sim 명령(ECU 쪽)
static bool arg_int(char **argv, int argc, int i, int lo, int hi, int *v) { return i < argc && cli_parse_int(argv[i], lo, hi, v); }

static bool parse_dtc(const char *t, uint16_t *out)
{
    static const char sys[] = "PCBU";
    if (strlen(t) != 5) return false;
    const char *p = strchr(sys, t[0] >= 'a' ? t[0] - 32 : t[0]);
    if (!p || t[1] < '0' || t[1] > '3') return false;
    uint32_t rest = 0;
    for (int i = 2; i < 5; i++) {
        char c = t[i];
        int h = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (h < 0) return false;
        rest = rest * 16 + (uint32_t)h;
    }
    *out = (uint16_t)(((p - sys) << 14) | ((t[1] - '0') << 12) | rest);
    return true;
}

void ecu_status_line(void)
{
    sct_outf("SIM,ecu,kbps=%d,noack=%d,rpm=%.0f,delay_ms=%.1f,pending=%d,added_dtc=%d,requests=%" PRIu32 ",violations=%" PRIu32 "\n", e.kbps, e.noack ? 1 : 0, e.rpm,
             (double)e.delay_us / 1000.0, e.pending_n, e.nadded, e.requests, e.violations);
}

bool ecu_cmd(int argc, char **argv)
{
    const char *c = argv[0];
    int v = 0;
    if (!strcmp(c, "rpm") && arg_int(argv, argc, 1, 0, 16000, &v)) {
        e.rpm = v;
        sct_outf("SIM,rpm,%d\n", v);
    } else if (!strcmp(c, "kbps") && arg_int(argv, argc, 1, 0, 100000, &v) && policy_speed_ok(v)) {
        e.kbps = v;
        sct_outf("SIM,kbps,%d\n", v);
    } else if (!strcmp(c, "noack") && argc > 1) {
        e.noack = !strcmp(argv[1], "on");
        sct_outf("SIM,noack,%d\n", e.noack ? 1 : 0);
    } else if (!strcmp(c, "delay") && arg_int(argv, argc, 1, 0, 10000, &v)) {
        e.delay_us = (int64_t)v * 1000;
        sct_outf("SIM,delay,%d\n", v);
    } else if (!strcmp(c, "pending") && arg_int(argv, argc, 1, 0, 100, &v)) {
        e.pending_n = v;
        int gap = (int)(ECU_PENDING_GAP_US / 1000);
        if (argc > 2 && !arg_int(argv, argc, 2, 1, 10000, &gap)) return false;
        e.pending_gap = (int64_t)gap * 1000;
        sct_outf("SIM,pending,%d,%d\n", v, gap);
    } else if ((!strcmp(c, "dropcf") || !strcmp(c, "cutcf") || !strcmp(c, "badsn") || !strcmp(c, "shortcf")) && arg_int(argv, argc, 1, 0, 1000, &v)) {
        if (c[0] == 'd') e.dropcf = v;
        else if (c[0] == 'c') e.cutcf = v;
        else if (c[0] == 'b') e.badsn = v;
        else e.shortcf = v;
        sct_outf("SIM,%s,%d\n", c, v);
    } else if (!strcmp(c, "stale") && argc > 1 && argc <= 8) {
        for (int i = 1; i < argc; i++)
            if (!cli_parse_hex_byte(argv[i], &e.stale[i - 1])) return false;
        e.stale_n = argc - 1;
        e.stale_on = true;
        sct_outf("SIM,stale,%d\n", e.stale_n);
    } else if (!strcmp(c, "adddtc") && argc > 1) {
        uint16_t code;
        if (!parse_dtc(argv[1], &code)) return false;
        if (argc > 3 && !strcmp(argv[2], "after") && arg_int(argv, argc, 3, 1, 100000, &v)) {
            e.add_code = code;
            e.add_after = v;
        } else if (argc == 2 && e.nadded < 8) {
            e.added[e.nadded++] = code;
        } else return false;
        sct_outf("SIM,adddtc,%s,%d\n", argv[1], e.add_after);
    } else if (!strcmp(c, "stream") && argc > 1) {
        uint32_t id;
        if (!strcmp(argv[1], "off")) {
            e.stream_on = false;
            sct_outf("SIM,stream,off\n");
            return true;
        }
        if (!cli_parse_hex_id(argv[1], &id) || !arg_int(argv, argc, 2, 1, 10000, &v)) return false;
        e.stream_on = true;
        e.stream_id = id;
        e.stream_period = (int64_t)v * 1000;
        e.stream_next = sim_time_us() + e.stream_period;
        e.stream_ack = argc > 3 && !strcmp(argv[3], "ack");
        sct_outf("SIM,stream,%03" PRIX32 ",%d,%s\n", id, v, e.stream_ack ? "ack" : "noack");
    } else return false;
    return true;
}
