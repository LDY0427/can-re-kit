# 2. Hardware and wiring

[한국어](ko/02-hardware.md)

## Choosing an adapter

| Device | Rough price | Listen (broadcast) | Request | `canre` setting | Notes |
|---|---|---|---|---|---|
| ELM327 (USB, Bluetooth) | $10–15 | Practically no | Yes | `-i elm327 -c COM5` | The most common. The adapter sets the answer rate, so you cannot measure the ECU's limit. Ours topped out at about 9 per second |
| CANable / slcan family | $25–40 | Yes | Yes | `-i slcan -c COM7` | `-i gs_usb` with candleLight firmware. Supports listen-only |
| PCAN-USB, Kvaser, … | $200+ | Yes | Yes | `-i pcan` / `-i kvaser` | Accurate error counters and timestamps |
| Raspberry Pi + MCP2515/MCP2518 | $15–30 | Yes | Yes | `-i socketcan -c can0` | Linux. Set listen-only with `ip link` |
| ESP32-S3 + CAN transceiver | about $10 | Yes | Yes | `-i esp32 -c COM9` | Flash [firmware/](../firmware/) from this repo. Can measure the ECU's own answer limit. We used an SN65HVD230 module |

If you are starting out, get **one ELM327 and one CANable**. The ELM327 tells you quickly whether anything answers; the CANable does listening and bitrate checks.

Any python-can interface works with `-i <name>`. See "Interfaces" in the python-can documentation.

## Wiring

```
 Adapter               Vehicle diagnostic connector
 CAN-H  ────────────── CAN-H
 CAN-L  ────────────── CAN-L
 GND    ────────────── Ground  (always connect it; without it the common-mode voltage floats and you get errors)
        (12 V only if the adapter needs it. Most USB adapters do not)
```

- **Twist H and L** (twisted pair) and keep them short.
- Check with a multimeter before the first connection.
  - Key OFF: resistance CAN-H ↔ CAN-L. About 60 Ω = two terminators, one at each end (a normal bus). About 120 Ω = one terminator. Open (kΩ or more) = no terminator.
  - Key ON: H and L each about 2.5 V to ground (quiet bus). While communicating, H moves between 2.5 and 3.5 V and L between 1.5 and 2.5 V. On a quiet bus you cannot tell H from L by voltage alone.
- **Termination**: a correct bus has one 120 Ω at each end, 60 Ω combined. If you see only 120 Ω, add 120 Ω on your side. If you see 60 Ω, do not. Cheap transceiver modules often have 120 Ω on the board, so measure the module on its own.
- Swapping H and L does no damage. If nothing answers, swap them.

## On motorcycles

- The diagnostic connector is under the seat or next to the battery, and is usually not a 16-pin OBD connector. Manufacturer-specific 4- and 6-pin connectors are common. See chapter 3.
- If you cannot get the mating connector, use back-probe pins or hook clips temporarily. Do not cut wires.
- Vibration is severe. For road logging, tie the connector down and fix the adapter in place.

## Software

```bash
pip install -e .          # this repository (pulls in python-can, numpy, pyyaml, pyserial)
canre --help
canre ping                # default is the simulated ECU (-i sim): practise the whole flow without a vehicle
```

If you want a nicer screen, use public tools such as SavvyCAN or Cangaroo (CANable) alongside. `canre` CSV logs also open in any spreadsheet.
