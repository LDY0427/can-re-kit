# 1. Concepts — the two ways values reach the bus

[한국어](ko/01-concepts.md)

## CAN frames

One CAN frame = an **ID** (11 or 29 bits) + **0–8 data bytes**. A lower ID has higher priority. The frame does not say who sent it; you can only guess from the ID.

If any receiver gets the frame correctly it sends an **ACK**. If no device is there to ACK, the sender retransmits the same frame over and over. This happens on a bus where only one ECU is left, as on many motorcycles (chapter 4).

Common bitrates: 500 kbps (most diagnostics and powertrain), 250 kbps (trucks, some motorcycles), 125 kbps, 1 Mbps.

## Two ways values arrive

| | Broadcast (periodic frames) | Request/response (diagnostic) |
|---|---|---|
| How | The ECU sends on its own at a fixed period; the dash and ABS consume it | The ECU answers only when a tester asks |
| How to find | Visible just by listening. Watch what each byte follows | You must send requests. Scan which identifiers answer |
| Strengths | Fast (10–100 ms period), you transmit nothing | Standard items have public formulas |
| Weaknesses | Formulas are private. With no dash or ABS there may be none at all | Needs a requesting device. Answers per second are limited |
| Output | A DBC file | A request list with formulas (`diag_signals` in this repo) |

**Listen first; if there is nothing, ask.** Our vehicle (a motorcycle engine ECU on its own) sent 0 broadcast frames in 200 seconds with the engine off, so we went to request/response.

## Structure of a diagnostic request (ISO 15765-4, OBD-II on CAN)

- Request ID: `7DF` (to every ECU) or `7E0`–`7E7` (to one ECU). Response ID: `7E8`–`7EF` (request ID + 8).
- The first data byte is the **length** (ISO-TP). When you type `canre req 21 21` the bus actually carries `7DF  02 21 21 00 00 00 00 00`. The tool adds the length byte.
- The first response byte = request service + `0x40`. A `21 21` request → `61 21 xx xx`. The trailing `xx xx` is the value.
- A refusal is `7F <service> <reason code>`. `12` = no such identifier, `11` = no such service, `22` = not possible in the current conditions, `33` = security access required, `78` = wait (answer coming).
- Answers longer than 7 bytes arrive as several frames (First Frame → the requester sends Flow Control → Consecutive Frames). Trouble-code lists are the usual example. `canre` handles this.

## Service numbers

| Service | Name | Content |
|---|---|---|
| `01` | OBD current data | Standard PIDs. `01 00` is a bitmap of "which of 01–20 are supported" |
| `03` `07` `0A` | Trouble codes | Stored / this drive cycle / permanent |
| `09` | Vehicle information | `09 02` = VIN (remove before sharing) |
| `21` | KWP2000 read data by local identifier | Manufacturer values, 1-byte identifier |
| `22` | UDS read data by identifier | Manufacturer values, 2-byte identifier |
| `19` | UDS DTC information | Trouble codes on newer ECUs |

**Standard PIDs are a ruler.** If you also read standard values with public formulas (coolant `01 05`, MAP `01 0B`, throttle `01 11`), you can find the meaning and formula of manufacturer identifiers by holding them against that ruler. That is the core of chapter 5.

## K-line is different

Many motorcycles (especially Japanese brands before the 2010s) use **K-line** (ISO 9141 / ISO 14230 KWP2000, a single wire), not CAN. The CAN features of this tool do not work on K-line. The service numbers (`21`, `1A`, …) and the decoding method (chapters 5–6) are the same. How to tell which bus you have: chapter 3.
