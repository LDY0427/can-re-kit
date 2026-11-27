# 7. Motorcycles and other vehicles

[한국어](ko/07-other-vehicles.md)

The method (listen → ask → hold against a reference → confirm → document) does not depend on the vehicle. What changes is the shape of the bus.

## Motorcycle checklist

- [ ] Found the diagnostic connector location and pinout in **at least two** sources, and checked it with a meter.
- [ ] Determined CAN or K-line (table in chapter 3). With K-line, this tool's CAN features do not apply.
- [ ] Listed the devices on the bus (ECU, ABS, dash, smart key, quickshifter, traction-control module).
- [ ] If the dash or ABS is on CAN: **listen** first. Wheel speed, gear and RPM are likely in broadcast frames.
- [ ] If the ECU is alone: there may be no broadcast frames. With a listen-only adapter there is no ACK and things can look odd (`listen --ack`).
- [ ] Do not be surprised if there is no standard RPM (`01 0C`). It may be in manufacturer identifiers (`21 xx`, `22 xxxx`).
- [ ] The battery is small. Keep key-ON time short, or connect a charger.
- [ ] Select a gear only with the bike on a stand and the rear wheel off the ground.

## Values that commonly turn up, and how to confirm them

| Value | Confirming test | Reference |
|---|---|---|
| RPM | Hold RPM steady for 20 s | Cumulative counter rate, optical tachometer |
| Throttle | Engine off, open and close slowly | Standard `01 11`, closed and wide-open end values |
| Coolant | Cold start until the fan runs | Standard `01 05`, thermometer |
| Gear | Engine off, turn the rear wheel by hand while changing gear | Integer values 1–6 plus neutral |
| Wheel speed | Rear wheel turning on a stand | GPS (riding), wheel revolutions × circumference |
| Side stand, clutch, brake | Press and release | 0/1 bits |
| Battery voltage | Before and after start | Multimeter |

## Other vehicles

| Vehicle | What differs | With this tool |
|---|---|---|
| Car | 16-pin OBD. A gateway behind the port often hides broadcast frames | Request/response as is. Broadcast on the bus behind the gateway |
| Scooter, ATV | Similar to motorcycles. Some have no diagnostics at all | Same procedure if there is a bus |
| E-bike, e-scooter, EV battery pack | BMS and motor controllers broadcast a lot. 250/500 kbps; 29-bit IDs are common | `listen`, `poll --record-all`, `analyze --reference "ID ..."`. Requests are non-standard and usually not used |
| Truck, agricultural (J1939) | 250 kbps, 29-bit, many public standard PGNs | `listen` shows the ID table. J1939 decoding is out of scope (use a dedicated tool) |
| Home-built vehicle (Formula Student etc.) | Stock ECU alone + home-built nodes | This procedure for the stock side; design the home-built side as a DBC from the start |

## When you install a device on the vehicle

If decoding is done and you build a device that reads values continuously (logger, dash, shift light):

- **Only one** device on the bus sends requests. Loggers and dashes only listen to the answers to that device's requests.
- Divide how often you ask by the ECU limit measured in chapter 4, step 6. Give a bigger share to values that must be fast, such as RPM.
- Work out the bus load: an 11-bit, 8-byte frame is about 135 bits with worst-case bit stuffing. At 500 kbps a request + answer pair takes about 0.54 ms.
