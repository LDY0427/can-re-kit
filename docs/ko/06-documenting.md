# 6. 기록하고 나누기 — 차종 프로필

[English](../06-documenting.md)

## 확인 수준을 섞지 않는다

신호표가 쓸모 있으려면 읽는 사람이 "이건 잰 것, 이건 짐작"을 가를 수 있어야 한다. 모든 값에 `confidence`를 단다.

| 수준 | 뜻 | `evidence`에 쓸 것 |
|---|---|---|
| `measured` | 내 차에서, 독립된 기준과 대조해 확인 | 무엇과 비교했나, 표본 수, 상관·잔차, 범위 |
| `inferred` | 내 기록의 패턴. 일부러 한 확인 시험은 아직 | 어떤 기록에서 어떤 패턴 |
| `reported` | 남의 주장(포럼, 글, 다른 모델) | 출처 종류와 몇 곳이 일치하는지 |
| `standard` | 공개 표준 식(SAE J1979 등) | 비워도 됨 |

`canre profile check`는 `measured` `inferred` `reported`에 근거가 비어 있으면 거부한다.

## 프로필 파일

`profiles/_template.yaml`을 복사해 `profiles/<브랜드>-<모델>-<연식>.yaml`로 만든다. 예: [ktm-390-duke-2016.yaml](../../profiles/ktm-390-duke-2016.yaml).

```yaml
diag_signals:
  - name: coolant_temp
    request: "21 22"     # 길이 바이트 없이
    byte: 0              # 값 안의 시작 위치(서비스·번호 되돌림 뒤)
    length: 1
    endian: big
    scale: 0.75          # 물리값 = raw × scale + offset
    offset: -48
    unit: C
    confidence: measured
    evidence: "표준 01 05와 29 C, 43~51 C에서 일치(r 0.99). 고온 구간은 아직"
```

모르는 것은 추측해서 채우지 말고 `open_questions`에 적는다.

```bash
canre profile check profiles/my-bike.yaml
canre profile table profiles/my-bike.yaml      # 마크다운 표
canre profile dbc   profiles/my-bike.yaml      # 상시 프레임 신호를 DBC로
canre poll "21 21" "21 22" --profile profiles/my-bike.yaml   # 물리값으로 보기
```

## 원본 기록

- 원본 CSV는 고치지 않고 두고, 해석은 따로 한다. 해석은 다시 할 수 있지만 측정은 다시 할 수 없다.
- 공유 전에 VIN(`09 02` 응답), 위치, 이름을 지운다.
- 기록마다 날짜, 차 상태(키 ON/엔진, 수온), 한 동작, 어댑터를 적는다.

## 기여

새 차종 프로필, 다른 어댑터에서 돌려 본 결과, 틀린 곳 지적 모두 환영한다. [CONTRIBUTING.md](../../CONTRIBUTING.md).
