# 6. Documenting and sharing — vehicle profiles

[한국어](ko/06-documenting.md)

## Never mix confidence levels

A signal table is only useful if a reader can tell "measured" from "guessed". Every value gets a `confidence`.

| Level | Meaning | What goes in `evidence` |
|---|---|---|
| `measured` | Checked on your vehicle against an independent reference | What it was compared with, sample count, correlation/residual, range |
| `inferred` | A pattern in your logs; no deliberate confirming test yet | Which log, which pattern |
| `reported` | Someone else's claim (forum, article, another model) | Kind of source, how many agree |
| `standard` | Public standard formula (SAE J1979 etc.) | May be empty |

`canre profile check` rejects `measured`, `inferred` and `reported` entries with empty evidence.

## Profile files

Copy `profiles/_template.yaml` to `profiles/<make>-<model>-<year>.yaml`. Example: [ktm-390-duke-2016.yaml](../profiles/ktm-390-duke-2016.yaml).

```yaml
diag_signals:
  - name: coolant_temp
    request: "21 22"     # without the length byte
    byte: 0              # start within the value (after the echoed service/identifier)
    length: 1
    endian: big
    scale: 0.75          # physical = raw × scale + offset
    offset: -48
    unit: C
    confidence: measured
    evidence: "matches standard 01 05 at 29 C and 43-51 C (r 0.99). Hot range not yet covered"
```

Do not guess to fill gaps; put unknowns under `open_questions`.

```bash
canre profile check profiles/my-bike.yaml
canre profile table profiles/my-bike.yaml      # Markdown table
canre profile dbc   profiles/my-bike.yaml      # broadcast signals as DBC
canre poll "21 21" "21 22" --profile profiles/my-bike.yaml   # see physical values
```

## Raw logs

- Keep raw CSVs untouched and analyse separately. Analysis can be redone; the measurement cannot.
- Remove the VIN (`09 02` answer), locations and names before sharing.
- For every log, note the date, vehicle state (key ON / engine, coolant temperature), the action and the adapter.

## Contributing

New vehicle profiles, results from other adapters and corrections are all welcome. See [CONTRIBUTING.md](../CONTRIBUTING.md).
