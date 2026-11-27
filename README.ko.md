# can-re-kit — 차량 CAN 역공학, 읽기만 하는 방법과 도구

학생 포뮬러 차량에 쓰는 오토바이 엔진(KTM 390 Duke, 2016)의 순정 ECU에서 회전수·스로틀·수온·흡기압을 받아야 했다. 그 ECU에 대한 공개 자료는 거의 없었고, 버스는 조용했다. 이 저장소는 그때 쓴 방법을 **다른 오토바이·자동차에도 쓸 수 있게** 정리한 것이다.

## 들어 있는 것

- **절차서**: "어느 선이 CAN-H인가"부터 "신호표 완성"까지, 단계마다 끝났다고 보는 기준과 함께 ([docs/](docs/)).
- **`canre`**: 듣기·묻기·훑기·대어 보기를 하는 파이썬 명령줄 도구. **읽기만 한다.** 쓰기·고장 코드 지우기·보안 접근·루틴·플래시는 코드에서 막혀 있고 끄는 옵션이 없다.
- **ESP32-S3 펌웨어**([firmware/](firmware/)): 1만 원 안팎으로 버스에 직접 무는 어댑터. ECU 자체의 응답 한계를 잰다. 장치 쪽에서도 같은 읽기 전용 규칙을 지킨다. 사례 차량에서 썼다(v2.0.0).
- **가상 오토바이 ECU**: 차 없이 책상에서 모든 단계를 연습한다.
- **차종 프로필**(YAML): 신호마다 확인 수준(`measured` 실측 / `inferred` 추정 / `reported` 남의 보고 / `standard` 표준)과 근거를 단다.
- **사례**: KTM 390 Duke ECU 해석 전 과정. 틀렸던 것까지.

## 바로 해 보기 (차 없이)

```bash
git clone https://github.com/LDY0427/can-re-kit && cd can-re-kit
pip install -e .
canre ping                         # 기본 인터페이스는 가상 ECU
canre pids                         # 표준 값. 오토바이처럼 표준 회전수가 없다
canre scan --engine-off            # 제조사 번호(서비스 21) 중 어디가 답하나
canre poll "21 3B" "01 11" "21 22" "01 05" -n 400 --interval 50 --log logs/demo.csv
canre analyze logs/demo.csv --reference "01 11"     # 스로틀과 같이 움직이는 바이트는?
```

실차에서는 어댑터를 붙인다: `-i slcan -c COM7`(CANable), `-i gs_usb`, `-i pcan`, `-i socketcan -c can0`, `-i elm327 -c COM5`, `-i esp32 -c COM9`(이 저장소의 펌웨어). python-can이 지원하는 인터페이스면 다 된다.

## 읽는 순서

1. [0. 안전과 법](docs/00-safety-and-law.md) ← 반드시 먼저
2. [1. 개념](docs/01-concepts.md) — 상시 프레임과 요청·응답, ISO-TP, 서비스 번호
3. [2. 장비와 배선](docs/02-hardware.md)
4. [3. 버스 찾기](docs/03-finding-the-bus.md) — 커넥터, CAN과 K-line 가르기, 속도
5. [4. 절차](docs/04-workflow.md) — 0~9단계
6. [5. 뜻 찾기](docs/05-decoding.md) — 대어 보기, 식 다듬기, 함정 8가지
7. [6. 기록하고 나누기](docs/06-documenting.md)
8. [7. 오토바이와 다른 차량](docs/07-other-vehicles.md)
9. [사례: KTM 390 Duke 2016](case-studies/ktm-390-duke-2016.md)
10. [ESP32-S3 펌웨어](firmware/README.md)

## 한계

- CAN만 된다. 예전 오토바이에 많은 K-line은 방법은 같지만 도구는 아직 없다.
- 요청은 11비트 OBD 주소만. 29비트·J1939는 듣기만 된다.
- `analyze`는 후보를 낸다. 프로필에 `measured`로 적으려면 일부러 한 확인 시험이 있어야 한다.

## 기여

새 차종 프로필, 다른 어댑터에서 돌려 본 결과, 틀린 곳 지적 환영. [CONTRIBUTING.md](CONTRIBUTING.md)

## 라이선스

코드(펌웨어 포함) MIT, 문서·프로필 CC BY 4.0. [LICENSE](LICENSE)
