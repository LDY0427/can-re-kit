# 0. Safety and the law — read this first

[한국어](ko/00-safety-and-law.md)

A vehicle bus is the wire that brakes, ABS and throttle share. The method in this repository **only reads**. Even so, ignore what follows and you can hurt the vehicle, people, or your warranty.

## What this tool will not do (blocked in code)

`canre` sends only read services: `01 02 03 07 09 0A 19 1A 21 22`. There is no way to send the services below, and deliberately no switch to turn the block off.

| Service | Meaning | Why it is blocked |
|---|---|---|
| `04`, `14` | Clear trouble codes | Destroys evidence; ruins service and inspection records |
| `10` | Session control | The door into programming and extended sessions |
| `11` | ECU reset | Stops the engine if the vehicle is moving |
| `27` | Security access | The step that unlocks immobilisers and write protection. Out of scope |
| `2E` `3B` `3D` `34`–`37` | Write, write memory, download | Bricks the ECU |
| `2F` `30` `31` `08` | Actuator tests, routines | Fans, pumps and injectors actually move |
| `28` `85` | Communication / DTC-setting control | Other modules miss faults |

Request IDs are limited to the OBD addresses (`7DF`, `7E0`–`7E7`). Open another one with `--allow-tx-id` only when you are sure of it.

## What can happen even when you only read

This happened on our vehicle ([case study](../case-studies/ktm-390-duke-2016.md)).

- **After 5 minutes of 50 requests per second, one more trouble code appeared (P0108).** We have not yet established whether the requests caused it. So read and write down the trouble codes before and after every measurement (`canre dtc`).
- A new code (P1505) also appeared while we were sending nothing. Do not blame a new code on your requests straight away; compare with **a run of the same length without requests**.

## Rules

1. **Start with key ON, engine OFF, vehicle on a stand.** When the engine runs, read only the identifiers that answered with the engine off.
2. **Never send a request you have not tried before while riding or driving.** Road logs use only a request list validated on the bench and at standstill.
3. **Check the pins with a multimeter before connecting.** The diagnostic connector also carries permanent 12 V. Wire it wrong and the adapter burns.
4. **Only one device on the bus sends requests.** If two ask at once, answers get mixed up.
5. **Never flash or reset a dev board while it is connected to the vehicle.** During that time the TX pin floats and can disturb the bus.
6. Run the laptop on battery or use an isolated adapter. Ground at one point.
7. Motorcycle batteries are small. Key ON for a long time drains them. Connect a charger or keep it short.

## Law and rights

- **Use this only on vehicles you own, or with the owner's permission.**
- Do not attach it secretly to someone else's vehicle, and do not use it for theft or immobiliser bypass, disabling emissions equipment, or odometer tampering. This repository does not answer those questions.
- Service manuals, diagnostic software and firmware are copyrighted. Do not put them in this repository. Share only values you measured and how you measured them.
- Remove VINs, number plates and locations from raw logs before sharing. ECUs answer `09 02` with the VIN.
- How modification affects warranty, inspection and insurance differs by country. Check for yourself.
