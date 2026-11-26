# Contributing / 기여하기

## Vehicle profiles / 차종 프로필

1. Copy `profiles/_template.yaml` to `profiles/<make>-<model>-<year>.yaml` (lowercase, hyphens).
2. Give every signal a `confidence` and, unless `standard`, an `evidence` line: what you compared it with, how many samples, the range covered.
3. Put what you do not know under `open_questions`. Do not guess to fill gaps.
4. `canre profile check profiles/<file>.yaml` must pass.
5. Do not include the VIN, location data, or text copied from service manuals or manufacturer software.

`profiles/_template.yaml`을 복사해 차종별 파일을 만든다. 모든 신호에 확인 수준과 근거를 달고, 모르는 것은 `open_questions`에 적는다. VIN·위치·정비서 원문은 넣지 않는다.

## Code / 코드

- `pip install -e ".[test]"` then `pytest -q`. New behaviour comes with a test, ideally against the simulator (`canre/sim.py`).
- The read-only policy in `canre/safety.py` is the point of this project. Pull requests that add write, clear, security-access, routine, reset or flash services, or an option to bypass the policy, will not be merged.
- Keep messages and docs plain: say what was measured and what was not.

## Docs / 문서

Docs are in Korean. English translations are welcome as `docs/en/<same name>.md`.
문서는 한국어가 원본이다. 영어 번역은 `docs/en/`에 같은 이름으로.
