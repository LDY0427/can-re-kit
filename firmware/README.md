# firmware — ESP32-S3 진단 CAN 읽기 펌웨어 (stock-can-tool v2)

1만 원 안팎의 ESP32-S3 개발보드와 CAN 트랜시버 모듈로 차량 진단 CAN에 **직접** 물리는 도구다. ELM327과 달리 어댑터가 응답 속도를 가리지 않아서 **ECU 자체의 한계**(초당 몇 번 답하는가, 지연이 얼마인가)를 잴 수 있다. 사례 차량에서 초당 100번, 지연 0.7~11.1 ms를 이 펌웨어로 쟀다([사례](../case-studies/ktm-390-duke-2016.md)).

- PC의 `canre`에서 어댑터로 쓴다: `canre -i esp32 -c COM9 ping`.
- 시리얼 터미널에서 명령을 직접 쳐도 된다(921600 baud).
- **읽기 요청만 보낸다.** 서비스 `01 02 03 07 09 0A 1A 21 22`만 허용하고, 요청 ID는 `7DF`, `7E0`~`7E7`만 쓴다. 쓰기·고장 코드 지우기·보안 접근·재시작은 보낼 방법이 없다(`components/sct_core/src/policy.c`). `canre`도 같은 검사를 한 번 더 한다.

## 확인 수준

| 무엇 | 상태 |
|---|---|
| 2.0.0 | 개발보드 + SN65HVD230 모듈로 실차(사례 차량)에서 사용. 듣기 200초, 엔진 가동 중 20 ms 간격 15,000번 요청 |
| 2.0.1 (지금 판) | 실차 측정에서 드러난 결함 수정. **PC 가상 시험과 ESP-IDF 빌드만 했다. 보드에 올려 본 적은 없다** |
| `rpmsrc` (공개판에서 더함) | 회전수 요청을 차마다 바꿀 수 있게 함. PC 가상 시험과 빌드만 했다 |
| `canre -i esp32` | 이 펌웨어의 가상 빌드(WebAssembly)를 상대로 시험. 실제 보드와 연결은 아직 |

## 준비물과 배선

| 부품 | 예 |
|---|---|
| ESP32-S3 개발보드 | ESP32-S3-DevKitC-1 계열 (N8R2, N16R8 등 아무거나) |
| 3.3 V CAN 트랜시버 모듈 | SN65HVD230, TJA1051T/3 모듈 |
| 꼬임선, 훅 클립 또는 뒤쪽 끼움 핀 | |

```
 ESP32-S3            트랜시버 모듈         차량
 GPIO13 (TX) ─────── CTX / TXD
 GPIO14 (RX) ─────── CRX / RXD
 3V3 ─────────────── 3V3
 GND ─────────────── GND ─────────────── 차량 접지
                     CANH ────────────── CAN-H
                     CANL ────────────── CAN-L
```

- 콘솔은 개발보드의 UART 쪽 USB 포트(UART0), 921600 baud.
- 모듈에 120 Ω 종단이 붙어 있는 경우가 많다. 모듈만 따로 H–L 저항을 재고, 버스 상태에 따라 떼거나 둔다([docs/02](../docs/02-hardware.md)).
- **보드에 USB 전원을 먼저 넣고 그다음에 CAN 선을 문다. 차에 물린 채로 굽거나 리셋하지 않는다.** 굽는 중과 리셋 직후에는 프로그램이 TX 핀을 잡지 못해 버스를 흔들 수 있다.
- GPIO19·20(USB)과 부팅 설정 핀은 CAN에 쓰지 않는다.

## 빌드와 올리기

ESP-IDF 5.5 기준.

```bash
cd firmware
idf.py set-target esp32s3      # 처음 한 번
idf.py build
idf.py -p COM9 flash monitor   # 차에서 떼고 한다
```

`sdkconfig.defaults.module`은 이 저장소에 회로가 없는 전용 모듈 보드(Rev.A0: TX GPIO21, 하드웨어 송신 잠금 스위치, 전압 측정)용 설정이다. 개발보드 조합에는 필요 없다.

## 명령

| 명령 | 하는 일 |
|---|---|
| `auto` | 듣기 전용으로 500·250·125·1000 kbps를 5초씩 |
| `listen <kbps> [초]` | 듣기 전용(ACK 안 줌)으로 받아 적기. 끝에 ID 표, 바이트 변화, 버스 부하 |
| `watch <kbps> [초]` | 일반 모드(ACK 줌)로 받아 적기. ECU 혼자 있는 버스에서 쓴다 |
| `ping [kbps]` | 0.3초 듣고 `01 00` 한 번. ACK가 오면 속도·결선이 맞다 |
| `req <바이트…>` | 읽기 요청 하나(길이 바이트는 적지 않는다). 예: `req 21 21` |
| `rpmsrc [바이트… [div N] \| none]` | 회전수를 읽는 요청. 기본 `01 0C` ÷ 4. 표준 회전수가 없는 ECU는 바꾼다(예: `rpmsrc 21 21`). `none`이면 엔진 정지를 사람이 확인한다 |
| `rpm` | `rpmsrc` 요청 하나 |
| `pids` | 표준 PID 지원 목록과 값 |
| `dtc` | 고장 코드 `03`·`07`·`0A` 읽기(지우지 않는다) |
| `scan21 [시작] [끝]` | 서비스 21 번호 훑기. 회전수 0을 확인한 뒤에만, 8개마다 회전수·고장 코드 재확인, 달라지면 멈춤 |
| `poll <ms> <횟수> <요청> [/ 요청…] [every N]` | 반복 요청. 지연 분포, 초당 횟수 요약 |
| `target <ID>` / `speed <kbps>` | 요청 ID(`7DF`, `7E0`~`7E7`), 요청 속도 |
| `print on\|off` / `status` / `hello` / `close` / `help` | |
| `loopback bench` | 책상 전용 자체 시험(조용한 버스에서 자기 수신 한 번) |
| 명령 끝에 `force` | 송신 전 듣기에서 버스 오류가 보여도 보낸다. 500 kbps에서만 |

명령이 도는 동안 들어온 입력은 "멈춤"으로 본다. 다음 명령은 `# done`을 본 뒤 보낸다.

## 줄 형식 (펌웨어 → PC)

한 줄에 하나, 쉼표 구분. `#`으로 시작하면 사람이 읽는 말. 명령마다 끝에 `# done`. 시각은 부팅 뒤 밀리초.

| 줄 | 내용 |
|---|---|
| `HELLO,stock-can-tool,<판>,<보드>,2,<빌드 날짜>` | 부팅과 `hello` |
| `RX,시각,ID,길이,데이터` | 받은 프레임(29비트 ID는 앞에 `x`) |
| `REQ,시각,요청 ID,길이,데이터,속도kbps` | 보낸 요청 |
| `ANS,시각,응답 ID,지연 ms,데이터` | 받은 응답(여러 프레임은 이어 붙여서) |
| `PARTIAL,받은/전체` | 여러 프레임 응답이 끊김 |
| `LID,번호,길이,데이터` / `LID,번호,거부,코드,뜻` | `scan21` 한 줄 |
| `POLL,시각,지연,데이터[,회전수]` / `POLLSUM` `POLLHIST` | `poll` 한 줄과 요약 |
| `DTC,…` | 고장 코드 |
| `IDX,ID,개수,DLC,주기…,바이트별 변화,첫 데이터,끝 데이터` | 듣기 끝의 ID 표 |
| `STAT,버스오류,REC,TEC,송신실패,놓친프레임,중재패배,ok\|passive\|busoff` | 컨트롤러 상태 |
| `ERR,번호,열쇠말` | 실패. 예: `ERR,1,no_response`, `ERR,4,blocked_service`, `ERR,17,rpm_unknown` |

## 안전 동작

1. 기본은 듣기 전용. 명령이 끝나면 CAN 드라이버를 내리고 TX 핀을 풀업 입력으로 둔다.
2. 일반 모드에 들어가기 전에 같은 속도로 0.3초 듣고, 버스 오류가 보이면 들어가지 않는다.
3. 요청은 단발 송신(재전송 없음). ACK 없음·bus-off면 바로 내린다.
4. 일반 모드로 듣는 동안 1초에 프레임 0개·버스 오류 50건 초과면 내린다.
5. `poll`은 ACK 없음이 3번 이어지면 멈춘다. 간격 5 ms 이상, 40,000번 이하.
6. 오류 수동 상태(TEC·REC ≥ 128)면 내리고 알린다.
7. `scan21`은 엔진 정지를 확인한 뒤에만 훑고, 고장 코드가 바뀌면 멈춘다. 코드는 지우지 않는다.

## 구조

```
 canre(PC) 또는 시리얼 터미널 ── 줄 ──┐
                                       │
 components/sct_core  (순수 C11, ESP-IDF·heap 안 씀)
   cli.c 명령 · diag.c 요청-응답 · isotp.c ISO-TP · busguard.c 버스 보호
   policy.c 허용 서비스 · stats.c 지연·ID 표 · dtc.c 고장 코드 · out.c 출력
                                       │ sct/port.h
            ┌──────────────────────────┴──────────────┐
   main/port_esp.c (ESP32-S3 TWAI)         port_sim/ (PC: 가상 버스 + 가상 ECU + 고장 주입)
```

같은 핵심 코드를 PC에서 WebAssembly로 빌드해 가상 ECU를 상대로 시험한다. 가상 ECU는 사례 차량의 실측 응답(표준 회전수 없음, `21 xx` 값, 고장 코드 프레임)을 흉내 내고, 응답 끊김·순번 틀림·ACK 없음·응답 대기·버스 오류를 일부러 넣을 수 있다.

## PC에서 시험 (보드 없이)

Python, Node.js, `pip install ziglang==0.13.0`(C 컴파일러로 `zig cc`를 쓴다).

```bash
python firmware/test/host_build.py                  # 단위 시험 + 골든 시험(88개)
python firmware/test/host_build.py tests --update-golden   # 출력이 바뀌는 수정 뒤. 바뀐 내용을 사람이 보고 커밋
node --no-warnings firmware/test/sim/run_sim.mjs    # 대화형 가상 펌웨어. sim help 로 고장 주입 명령
pytest tests/test_firmware_link.py                  # canre ↔ 가상 펌웨어
```
