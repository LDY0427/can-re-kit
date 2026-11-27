# firmware — ESP32-S3 diagnostic CAN reader (stock-can-tool v2)

[한국어](README.ko.md)

A tool that connects **directly** to a vehicle's diagnostic CAN with a ~$10 ESP32-S3 dev board and a CAN transceiver module. Unlike an ELM327 the adapter does not hide the answer timing, so you can measure **the ECU's own limits** (how many answers per second, how much latency). On the case-study vehicle this firmware measured 100 per second and 0.7–11.1 ms ([case study](../case-studies/ktm-390-duke-2016.md)).

- Use it as an adapter from `canre` on the PC: `canre -i esp32 -c COM9 ping`.
- Or type commands directly in a serial terminal (921600 baud).
- **It sends read requests only.** Only services `01 02 03 07 09 0A 1A 21 22` and request IDs `7DF`, `7E0`–`7E7` are allowed. There is no way to send write, clear-codes, security access or reset (`components/sct_core/src/policy.c`). `canre` checks again on its side.
- The firmware's console messages are in Korean for now; the machine-readable lines (`ANS`, `RX`, `ERR`, …) are language-independent.

## Verification level

| What | Status |
|---|---|
| 2.0.0 | Used on the case-study vehicle with a dev board + SN65HVD230 module. 200 s listening, 15,000 requests at 20 ms spacing with the engine running |
| 2.0.1 (current) | Fixes for defects found in the vehicle measurements. **Tested in PC simulation and built with ESP-IDF only. Not yet flashed to a board** |
| `rpmsrc` (added for the public release) | RPM request configurable per vehicle. Simulation and build only |
| `canre -i esp32` | Tested against this firmware's simulated (WebAssembly) build. Not yet with a real board |

## Parts and wiring

| Part | Example |
|---|---|
| ESP32-S3 dev board | ESP32-S3-DevKitC-1 family (N8R2, N16R8, any) |
| 3.3 V CAN transceiver module | SN65HVD230, TJA1051T/3 module |
| Twisted pair, hook clips or back-probe pins | |

```
 ESP32-S3            Transceiver module    Vehicle
 GPIO13 (TX) ─────── CTX / TXD
 GPIO14 (RX) ─────── CRX / RXD
 3V3 ─────────────── 3V3
 GND ─────────────── GND ─────────────── Vehicle ground
                     CANH ────────────── CAN-H
                     CANL ────────────── CAN-L
```

- The console is the dev board's UART-side USB port (UART0), 921600 baud.
- Modules often carry a 120 Ω terminator. Measure the module's H–L resistance on its own and remove or keep it depending on the bus ([docs/02](../docs/02-hardware.md)).
- **Power the board over USB first, then connect the CAN wires. Never flash or reset it while it is connected to the vehicle.** While flashing and right after reset the program does not hold the TX pin and can disturb the bus.
- Do not use GPIO19/20 (USB) or boot-strapping pins for CAN.

## Build and flash

ESP-IDF 5.5.

```bash
cd firmware
idf.py set-target esp32s3      # once
idf.py build
idf.py -p COM9 flash monitor   # with the board disconnected from the vehicle
```

`sdkconfig.defaults.module` is for a dedicated module board whose schematic is not in this repository (Rev.A0: TX on GPIO21, hardware transmit-lock switch, voltage measurement). The dev-board combination does not need it.

## Commands

| Command | What it does |
|---|---|
| `auto` | Listen-only at 500, 250, 125, 1000 kbps, 5 s each |
| `listen <kbps> [s]` | Record in listen-only mode (no ACK). Ends with the ID table, byte changes, bus load |
| `watch <kbps> [s]` | Record in normal mode (ACKs). Use on a bus where the ECU is alone |
| `ping [kbps]` | Listen 0.3 s, then send `01 00` once. An ACK means bitrate and wiring are right |
| `req <bytes…>` | One read request (without the length byte). E.g. `req 21 21` |
| `rpmsrc [bytes… [div N] \| none]` | The request used to read RPM. Default `01 0C` ÷ 4. Change it for ECUs without standard RPM (e.g. `rpmsrc 21 21`). With `none` a person confirms the engine is off |
| `rpm` | One `rpmsrc` request |
| `pids` | Supported standard PIDs and their values |
| `dtc` | Read trouble codes `03`, `07`, `0A` (never clears) |
| `scan21 [start] [end]` | Scan service 21 identifiers. Only after confirming RPM 0; re-checks RPM and trouble codes every 8; stops if they change |
| `poll <ms> <count> <req> [/ req…] [every N]` | Repeated requests. Latency distribution and rate summary |
| `target <ID>` / `speed <kbps>` | Request ID (`7DF`, `7E0`–`7E7`), request bitrate |
| `print on\|off` / `status` / `hello` / `close` / `help` | |
| `loopback bench` | Desk-only self test (one self-reception on a quiet bus) |
| `force` at the end of a command | Send even if bus errors were seen in the pre-send listen. 500 kbps only |

Input received while a command runs is treated as "stop". Send the next command after you see `# done`.

## Line format (firmware → PC)

One per line, comma-separated. Lines starting with `#` are for people. Every command ends with `# done`. Time is milliseconds since boot.

| Line | Content |
|---|---|
| `HELLO,stock-can-tool,<version>,<board>,2,<build date>` | At boot and on `hello` |
| `RX,time,ID,length,data` | Received frame (29-bit IDs prefixed with `x`) |
| `REQ,time,request ID,length,data,bitrate kbps` | Request sent |
| `ANS,time,response ID,latency ms,data` | Answer received (multi-frame answers joined) |
| `PARTIAL,received/total` | A multi-frame answer was cut off |
| `LID,ID,length,data` / `LID,ID,refused,code,meaning` | One `scan21` line |
| `POLL,time,latency,data[,rpm]` / `POLLSUM` `POLLHIST` | One `poll` line and summaries |
| `DTC,…` | Trouble codes |
| `IDX,ID,count,DLC,period…,per-byte changes,first data,last data` | ID table at the end of listening |
| `STAT,bus errors,REC,TEC,tx failures,missed frames,arbitration lost,ok\|passive\|busoff` | Controller state |
| `ERR,number,key` | Failure. E.g. `ERR,1,no_response`, `ERR,4,blocked_service`, `ERR,17,rpm_unknown` |

## Safety behaviour

1. Listen-only by default. When a command ends the CAN driver is stopped and the TX pin becomes a pulled-up input.
2. Before entering normal mode it listens 0.3 s at the same bitrate and does not enter if it sees bus errors.
3. Requests are single-shot (no retransmission). No ACK or bus-off → stop immediately.
4. While listening in normal mode, 0 frames and more than 50 bus errors in one second → stop.
5. `poll` stops after 3 consecutive missing ACKs. Interval 5 ms or more, at most 40,000 requests.
6. Error-passive state (TEC or REC ≥ 128) → stop and report.
7. `scan21` scans only after confirming the engine is off, and stops if trouble codes change. It never clears them.

## Structure

```
 canre (PC) or serial terminal ── lines ──┐
                                          │
 components/sct_core  (plain C11, no ESP-IDF, no heap)
   cli.c commands · diag.c request/response · isotp.c ISO-TP · busguard.c bus protection
   policy.c allowed services · stats.c latency/ID table · dtc.c trouble codes · out.c output
                                          │ sct/port.h
            ┌─────────────────────────────┴──────────────┐
   main/port_esp.c (ESP32-S3 TWAI)          port_sim/ (PC: virtual bus + virtual ECU + fault injection)
```

The same core is built for the PC as WebAssembly and tested against a virtual ECU. The virtual ECU mimics the case-study vehicle's measured answers (no standard RPM, `21 xx` values, trouble-code frames) and can inject cut-off answers, wrong sequence numbers, missing ACKs, response-pending and bus errors on purpose.

## Testing on the PC (no board)

Python, Node.js and `pip install ziglang==0.13.0` (uses `zig cc` as the C compiler).

```bash
python firmware/test/host_build.py                         # unit + golden tests (88)
python firmware/test/host_build.py tests --update-golden   # after a change that alters output; review the diff before committing
node --no-warnings firmware/test/sim/run_sim.mjs           # interactive simulated firmware; 'sim help' for fault injection
pytest tests/test_firmware_link.py                         # canre <-> simulated firmware
```
