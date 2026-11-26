# can-re-kit

**A read-only playbook and toolkit for reverse-engineering a vehicle's CAN bus — cars, motorcycles, and home-built vehicles.**

[한국어 안내 → README.ko.md](README.ko.md)

We needed engine RPM, throttle, coolant and MAP from a stock motorcycle ECU (KTM 390 Duke, 2016) for a student race car. Nothing was published for that ECU, and the bus was silent. This repository is the method we ended up with, turned into something you can reuse on your own vehicle:

- **A step-by-step workflow** from "which wire is CAN-H?" to a documented signal table ([docs/](docs/), Korean).
- **`canre`**, a small Python CLI that sniffs, queries, scans and correlates — and **can only read**. Write, clear-codes, security-access, routine and flash services are blocked in code, with no override.
- **A simulated motorcycle ECU** so you can practise every step at your desk.
- **Vehicle profiles** in YAML where every signal carries a confidence level (`measured` / `inferred` / `reported` / `standard`) and its evidence.
- **A full case study** of the KTM 390 Duke ECU, including what went wrong.

## Quick start (no vehicle needed)

```bash
git clone https://github.com/LDY0427/can-re-kit && cd can-re-kit
pip install -e .
canre ping                         # the default interface is the simulator
canre pids                         # standard OBD-II values (no standard RPM, like many bikes)
canre scan --engine-off            # which manufacturer IDs (service 21) answer?
canre poll "21 3B" "01 11" "21 22" "01 05" -n 400 --interval 50 --log logs/demo.csv
canre analyze logs/demo.csv --reference "01 11"     # which unknown bytes follow throttle?
```

On a vehicle, add an interface: `-i slcan -c COM7` (CANable), `-i gs_usb`, `-i pcan`, `-i socketcan -c can0`, or `-i elm327 -c COM5`. Any [python-can](https://python-can.readthedocs.io/) interface works.

## Commands

| command | what it does |
|---|---|
| `listen` | record raw frames, summarise IDs, periods and which bytes change |
| `bitrate` | listen-only at 500/250/125/1000 kbps to find the bus speed |
| `ping` | send a single `01 00` and see if an ECU answers |
| `pids` / `dtc` | read supported standard values / trouble codes (never clears) |
| `req` | one read request, e.g. `req 21 21` |
| `scan` | try every identifier of service `21` or `22`, re-reading DTCs as it goes; stops if they change |
| `poll` | repeat requests, log to CSV; `--record-all` also logs broadcast frames |
| `analyze` | rank every unknown byte window against a known reference: correlation, fitted formula, chance threshold |
| `profile` | validate a vehicle profile, print it as a table, export broadcast signals as DBC |

## Read this first

[docs/00-safety-and-law.md](docs/00-safety-and-law.md). Use this only on vehicles you own or have permission to work on. Even read-only requests can coincide with new trouble codes — in our case they did, and we still don't know whether they caused it.

## Contents

| | |
|---|---|
| [docs/01-concepts.md](docs/01-concepts.md) | broadcast vs request/response, ISO-TP, service numbers |
| [docs/02-hardware.md](docs/02-hardware.md) | adapters, wiring, termination |
| [docs/03-finding-the-bus.md](docs/03-finding-the-bus.md) | connectors, CAN vs K-line, bitrate |
| [docs/04-workflow.md](docs/04-workflow.md) | the procedure, with a "done when" for every step |
| [docs/05-decoding.md](docs/05-decoding.md) | correlating, cleaning up formulas, the traps |
| [docs/06-documenting.md](docs/06-documenting.md) | profiles and confidence levels |
| [docs/07-other-vehicles.md](docs/07-other-vehicles.md) | motorcycles, EVs, cars, trucks |
| [case-studies/ktm-390-duke-2016.md](case-studies/ktm-390-duke-2016.md) | the worked example |
| [profiles/](profiles/) | per-vehicle signal tables — contributions welcome |

## Limitations

- CAN only. Many older motorcycles use K-line; the method applies, the tool does not (yet).
- Requests are 11-bit OBD addressing. 29-bit and J1939 frames can be listened to but not queried.
- `analyze` finds candidates, not truths. Every `measured` entry in a profile needs a deliberate confirming test.

## License

Code: MIT. Documentation and profiles: CC BY 4.0. See [LICENSE](LICENSE).
