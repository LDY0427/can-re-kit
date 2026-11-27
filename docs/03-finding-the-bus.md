# 3. Finding the bus — what flows where

[한국어](ko/03-finding-the-bus.md)

## 1) Documents first

1. In the service manual and wiring diagram, find the ECU pins named `CAN-H`, `CAN-L`, `K-LINE`, `DIAG`. Buy or borrow the manual (do not copy its contents into a public repository).
2. Find the diagnostic connector pinout. Check whether **several sources agree**. With only one source, record it as `reported`.
3. List the devices on the bus: engine ECU, ABS, dash, smart key, quickshifter, aftermarket modules. This list decides whether broadcast frames are likely.

## 2) Where to look first, by vehicle type

| Type | Diagnostic connector | Bus | Notes |
|---|---|---|---|
| Car (roughly 2008 on) | OBD-II 16-pin under the dash | pin 6 CAN-H, 14 CAN-L, 16 12 V, 4/5 ground | Many cars have a gateway behind the port, so broadcast frames are not visible there |
| Motorcycle | Under the seat or near the battery; manufacturer-specific connectors are common | CAN or K-line | Table below and chapter 7 |
| Race / modified vehicle | Inside the harness | May be the engine ECU alone | With ABS and dash removed there may be no broadcast frames |

Motorcycle diagnostic connectors differ by model and year. Rough trends from public sources (**all `reported`: check your own vehicle with a meter**):

- Recent models from some European brands use CAN diagnostics and are connected to the dash and ABS over CAN; broadcast frames are widely reported.
- Older Japanese models often use K-line diagnostics (manufacturer 4- and 6-pin connectors).
- American cruisers split between J1850 and CAN depending on year.
- ISO 19689 defines a motorcycle diagnostic connector, but not every vehicle follows it.

Please record what you find per model in `profiles/` (chapter 6).

## 3) Telling them apart with a meter

| Measurement | CAN | K-line |
|---|---|---|
| Resistance between two wires (key OFF) | about 60 Ω or 120 Ω | Single wire, no pair |
| Key ON voltage (idle) | Both wires about 2.5 V | About 12 V (pulled up to battery) |
| While communicating | H 2.5↔3.5 V and L 2.5↔1.5 V, moving in opposite directions | 12 V ↔ 0 V |

With an oscilloscope, one bit width gives the bitrate directly: 2 µs = 500 kbps, 4 µs = 250 kbps.

## 4) Listen → ask

```bash
canre bitrate -i slcan -c COM7          # listen-only at 500/250/125/1000 kbps, 5 s each
canre listen  -i slcan -c COM7 -s 60    # 60 s at the right bitrate, then the ID table
canre ping    -i slcan -c COM7          # if quiet: ask 01 00 exactly once
```

- In `bitrate`, the rate with frames and zero errors is the answer.
- If nothing shows at any rate, the bus is quiet (like ours). Then `ping`. If it answers, bitrate and wiring are right.
- If `ping` also gets nothing: check key ON, `--bitrate 250`, swap H/L, termination, ground, in that order. If still nothing, it is probably K-line.
