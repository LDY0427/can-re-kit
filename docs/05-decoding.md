# 5. Finding meaning — hold it against something known

[한국어](ko/05-decoding.md)

## Method

What `canre analyze` does:

1. **Time alignment.** Requests go out one after another, so each identifier is read at a different moment. The reference is linearly interpolated to each candidate sample's time. Samples whose surrounding reference points are more than `--max-gap` (default 3 s) apart are dropped.
2. **Byte windows.** At each position of the answer it extracts 1 byte (`u8`), 2 bytes big-endian (`u16be`) and 2 bytes little-endian (`u16le`).
3. **Correlation.** Pearson r (straight-line relation) and Spearman ρ (order only). Low r with high ρ can mean a non-linear relation (lookup table, thermistor).
4. **Fit.** Least squares: `reference = slope × raw + offset`.
5. **Chance threshold.** With m windows compared, it reports the |r| two unrelated signals would exceed, with a Bonferroni correction. Neighbouring time-series samples resemble each other, so the effective sample count is smaller; even above the threshold it is only a candidate.

## Cleaning up formulas so a person can read them

Fitted slopes usually sit near a tidy number. ECUs like powers of two and simple offsets.

| Fitted | Cleaned | Common reason |
|---|---|---|
| 0.2499 × raw | raw / 4 | RPM in 0.25 rpm units |
| 0.003906 × raw | raw / 256 | high byte is the integer part |
| 0.749 × raw − 47.8 | 0.75 × raw − 48 | temperature (0.75 °C units, offset) |
| 1.0 × raw − 40 | raw − 40 | standard temperature (J1979) |
| 0.392 × raw | raw × 100/255 | 0–255 mapped to 0–100 % |

Recompute with the cleaned formula and check that the residual does not grow.

## Traps

1. **Everything that moves together gets caught together.** RPM, MAP and throttle rise together while riding. Separate them using stretches where only one moves (engine off with throttle only, steady RPM). Even in the simulator, using throttle as the reference brings MAP up at r 0.85.
2. **Things that only increase correlate with each other.** While the engine warms up, coolant and cumulative counters both only rise. Be suspicious of candidates marked `[counter-like]`. Run the simulator with `--reference "01 05"` and the counter `21 64` comes second at r 0.98.
3. **Endianness and byte boundaries.** If only the high byte of a 2-byte value is caught as `u8`, correlation is still high. If both `u16be` and `u8` show up for the same identifier, the 2-byte one is probably right (compare residuals).
4. **Do not reuse tables from another year or model.** Even within a brand, a different ECU generation uses different identifiers. Treat published ID tables as `reported` and refit them on your vehicle.
5. **Strange numbers in someone else's formula.** A forum "divide by 264.12 and it matches" often has measurement error folded into the formula. Put the physically sensible formula (256) next to it and decide with a confirming test.
6. **The sensor may not be connected to the ECU.** If a value is stuck, check the wiring first. On our vehicle the oxygen sensor had been moved from the ECU to an aftermarket module, so the ECU's O2 value was stuck at 0.46 V.
7. **What the ECU computes is not what the actuators get.** If a fuel/ignition module sits after the ECU, the injection and ignition values the ECU reports are not what reaches the injector and coil. Sensor values (throttle, MAP, coolant, RPM) are unaffected.
8. **An ELM327's answers per second are not the ECU's limit.** Measure again with an adapter connected directly to the bus.

## Cumulative counters are a good cross-check

A 4-byte value that is 0 with the engine off and only rises while running may be a crankshaft revolution counter. Its increase per minute equals the mean RPM, so it independently checks an RPM candidate's scale. We confirmed `21 21 ÷ 4` to within ±5 % this way. To settle it exactly, hold RPM at one value for 20 s or more.

## Decoding broadcast frames

Use `poll --record-all` to put broadcast frames and standard values in the same log, then the same method:

```bash
canre analyze logs/run.csv --reference "01 0C"          # if the vehicle supports standard RPM
canre analyze logs/run.csv --reference "ID 120:u16be"   # a broadcast value already found, as the ruler
```

Broadcast bytes also include counters (`c`), checksums (last byte, changes every frame) and bit flags (brake or clutch switch: 0/1). For flags, pressing and releasing the switch while watching `last data` in the `listen` table is faster.
