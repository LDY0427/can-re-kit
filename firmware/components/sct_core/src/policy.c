// 송신 정책 표. 허용은 표에 "읽기"로 적힌 서비스뿐이다. 표에 없는 번호(0x00~0xFF 나머지)는 모두 거절한다.
// 서비스 번호의 뜻: 01~0A = SAE J1979(OBD), 1A·21 = ISO 14230-3(KWP2000), 그 밖 = ISO 14229-1(UDS)·ISO 14230-3.
#include "sct/policy.h"

#include <stddef.h>

typedef struct {
    uint8_t svc;
    bool read; // true = 보낸다
    const char *name;
} svc_row_t;

static const svc_row_t k_services[] = {
    {0x01, true, "현재 값"},
    {0x02, true, "고장 시점 값"},
    {0x03, true, "고장 코드 읽기"},
    {0x04, false, "고장 코드 지우기"},
    {0x05, false, "산소 센서 시험 결과"},
    {0x06, false, "감시 시험 결과"},
    {0x07, true, "미확정 고장 코드 읽기"},
    {0x08, false, "구동 시험(장치 제어)"},
    {0x09, true, "차량 정보"},
    {0x0A, true, "영구 고장 코드 읽기"},
    {0x10, false, "진단 세션 바꾸기"},
    {0x11, false, "ECU 재시작"},
    {0x14, false, "고장 코드 지우기"},
    {0x17, false, "고장 코드 상태 읽기(KWP)"},
    {0x18, false, "고장 코드 읽기(KWP)"},
    {0x19, false, "고장 코드 정보 읽기(UDS)"},
    {0x1A, true, "ECU 식별(KWP)"},
    {0x20, false, "진단 끝내기(KWP)"},
    {0x21, true, "로컬 번호로 읽기(KWP)"},
    {0x22, true, "번호로 읽기"},
    {0x23, false, "메모리 주소로 읽기"},
    {0x27, false, "보안 접근"},
    {0x28, false, "통신 제어"},
    {0x2C, false, "동적 번호 정의"},
    {0x2E, false, "번호로 쓰기"},
    {0x2F, false, "입출력 제어"},
    {0x30, false, "입출력 제어(KWP)"},
    {0x31, false, "루틴 실행"},
    {0x34, false, "내려받기 요청"},
    {0x35, false, "올려받기 요청"},
    {0x36, false, "데이터 전송"},
    {0x37, false, "전송 끝"},
    {0x3B, false, "로컬 번호로 쓰기(KWP)"},
    {0x3D, false, "메모리 주소로 쓰기"},
    {0x3E, false, "연결 유지"},
    {0x85, false, "고장 코드 기록 설정"},
    {0x86, false, "이벤트 응답"},
    {0x87, false, "통신 속도 바꾸기"},
};

static const svc_row_t *find(uint8_t svc)
{
    for (size_t i = 0; i < sizeof k_services / sizeof k_services[0]; i++)
        if (k_services[i].svc == svc) return &k_services[i];
    return NULL;
}

bool policy_service_allowed(uint8_t svc)
{
    const svc_row_t *r = find(svc);
    return r && r->read;
}

const char *policy_service_name(uint8_t svc)
{
    const svc_row_t *r = find(svc);
    return r ? r->name : "이름 없는 서비스";
}

bool policy_req_id_ok(uint32_t id)
{
    return id == SCT_REQ_ID_FUNCTIONAL || (id >= SCT_REQ_ID_PHYS_FIRST && id <= SCT_REQ_ID_PHYS_LAST);
}

bool policy_speed_ok(int kbps) { return kbps == 1000 || kbps == 500 || kbps == 250 || kbps == 125 || kbps == 100; }

bool policy_force_ok(int kbps) { return kbps == SCT_CONFIRMED_KBPS; }

const char *policy_nrc_name(uint8_t c)
{
    switch (c) { // v1 nrc_name과 같은 문구
    case 0x10: return "일반 거부";
    case 0x11: return "지원하지 않는 서비스";
    case 0x12: return "지원하지 않는 하위 기능/번호";
    case 0x13: return "길이·형식 오류";
    case 0x22: return "조건이 맞지 않음";
    case 0x31: return "범위 밖 번호";
    case 0x33: return "보안 접근 필요";
    case 0x78: return "응답 대기";
    case 0x7F: return "이 세션에서 지원 안 함";
    case 0x80: return "이 세션에서 지원 안 함(KWP)";
    default: return "기타";
    }
}
