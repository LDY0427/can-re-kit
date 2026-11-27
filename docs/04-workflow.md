# 4. Workflow — from nothing to a signal table

[한국어](ko/04-workflow.md)

Every step has a "done when". If you cannot meet it, do not move on. Practise every command first with `-i sim` (the default).

```
0 desk practice → 1 wiring check → 2 listen → 3 first request → 4 standard values → 5 scan manufacturer IDs
→ 6 record values → 7 find meaning → 8 confirming test → 9 document and share
```

## 0. Desk practice (no vehicle)

```bash
canre ping
canre pids
canre scan --engine-off --start 00 --end FF
canre poll "21 21" "01 11" "21 3B" "01 05" "21 22" -n 400 --interval 50 --log logs/practice.csv
canre analyze logs/practice.csv --reference "01 11"
```

The simulated ECU behaves like ours (no standard RPM, RPM in a manufacturer identifier). **Done when:** all commands run and `analyze` finds `21 3B` as throttle.

## 1. Wiring check (key OFF → key ON)

Measure and write down the resistance and voltages from chapter 2. **Done when:** termination state (60 / 120 / open) and H/L voltages are recorded.

## 2. Listen (key ON, engine off)

```bash
canre bitrate -i slcan -c COM7
canre listen  -i slcan -c COM7 -s 60
```

- The `bytes` column in the ID table: `.` never changes, `*` changes sometimes, `c` changes every frame (counter or checksum candidate).
- If there are frames, broadcast decoding is possible. In step 7 you hold them against references as `ID xxx`.
- Listen once more with the engine running (some frames appear only after start).
- **If the ECU is alone on the bus**, a listen-only adapter gives no ACK. Even if the ECU sends something, without an ACK it repeats the same frame or goes into an error state. Listen once more with `--ack` (normal mode).

**Done when:** bitrate, whether broadcast exists, and the ID list are written down.

## 3. First request

```bash
canre dtc  -i slcan -c COM7          # write down the trouble codes first
canre ping -i slcan -c COM7          # 01 00, once
```

**Done when:** the response ID (usually `7E8`) and latency are written down, and the trouble-code list is saved as a photo or file.

## 4. Read standard values

```bash
canre pids -i slcan -c COM7
```

Write down the supported list and values. These are the "ruler" for step 7. Some motorcycles do not support standard RPM (`01 0C`); ours did not.

**Done when:** you have the supported PID list and each value is plausible (coolant ≈ real temperature, closed throttle ≈ 0 %).

## 5. Scan manufacturer identifiers (key ON, engine off only)

```bash
canre scan -i slcan -c COM7 --engine-off --service 21 --start 00 --end FF
canre scan -i slcan -c COM7 --engine-off --service 22 --start F180 --end F1A0   # UDS vehicles: narrow the range
```

- Trouble codes are read before and after the scan and every 16 identifiers. If they change, the scan stops.
- Default gap between requests is 20 ms. Do not hurry.
- Service `22` has 65,536 identifiers. Do not scan them all; start from known ranges (`F1xx` identification, etc.) and keep it narrow.
- Write down which identifiers answered and how long each answer is. The length is the first hint about the value (1 byte = temperature or flag, 2 bytes = RPM or pressure, 4 bytes = cumulative counter).

**Done when:** you have the list of answering identifiers with lengths, and trouble codes before and after are the same (if not, write that down).

## 6. Record values

Start with the engine off; with the engine running, read only identifiers that answered with the engine off.

```bash
canre poll -i slcan -c COM7 "21 21" "21 22" "21 3B" "21 4A" "01 05" "01 0B" "01 11" \
     --interval 100 -n 1200 --record-all --log logs/idle_to_rev.csv
```

- **Always include known values (standard PIDs).** Without a ruler you cannot find meaning.
- Move one thing at a time. Engine off, open and close only the throttle; after start, raise only the RPM; cold and warming up. If several values rise together you cannot separate them (chapter 5, trap 1).
- Note what you did and when ("12 s: wide open, 20 s: closed").
- `--record-all` also puts broadcast frames seen during polling into the same log.
- To measure how many requests per second the ECU can take, shrink the interval 100 → 50 → 20 → 10 ms and find where failures start. A figure measured with an ELM327 is the adapter's limit; do not record it as the ECU's.

**Done when:** every action is in the log and you have notes.

## 7. Find meaning

```bash
canre analyze logs/idle_to_rev.csv --reference "01 11"              # bytes that follow throttle
canre analyze logs/idle_to_rev.csv --reference "01 05"              # coolant
canre analyze logs/idle_to_rev.csv --reference "21 21:u16be*0.25"   # a value you already found is also a ruler
```

The output is byte position, layout, correlation (r, ρ) and a fitted formula (`ref = slope × raw + offset`). Even above the chance threshold it is only a **candidate**. How to read it, and the traps: chapter 5.

## 8. Confirming test

For each candidate, deliberately run a test that moves only that value. Examples: hold RPM steady for 20 s and compare with a cumulative counter's rate; compare coolant from a cold start to hot. Comparing with an independent instrument (thermometer, pressure gauge, tachometer) is even better. If it passes, it is `measured`.

## 9. Document and share

Write it into `profiles/<vehicle>.yaml` (chapter 6). Keep raw logs with the VIN removed. Write what you still do not know under `open_questions`.
