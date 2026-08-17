# Murphy M4 hardware knowledge base

This directory is the canonical hardware source for the Murphy M4 work in this
repository. Firmware, simulator and QEMU board-model changes should link back to
facts recorded here instead of duplicating board assumptions in handoff notes or
ad-hoc code comments.

## Layout

- `schematics/` — source schematics and provenance notes.
- `murphy-m4/FACTS.md` — verified board facts extracted from schematics and
  cross-checked against firmware/SDK behavior.
- `murphy-m4/NETS.md` — named electrical nets grouped by peripheral/bus.
- `murphy-m4/EMULATOR_MODEL.md` — mapping from real hardware to the QEMU/M4Sim
  model, including what is modeled, stubbed, or still missing.

## Evidence levels

Every non-obvious board fact should carry one of these labels:

- **SCH** — directly visible in the archived schematic.
- **SDK** — derived from the FreeInk/Murphy SDK or board configuration.
- **FW** — observed from production firmware behavior/logging.
- **MEASURED** — confirmed on a physical device with a meter, logic analyzer,
  scope, flash/eFuse dump, or other measurement.
- **INFERRED** — plausible but not yet verified; do not use as a hard emulator
  contract until confirmed.

When sources disagree, preserve both observations and record the conflict rather
than silently choosing one.

## Emulator rule

The binary-level emulator target is an **unmodified production Murphy M4 flash
image**. `M4_QEMU_BUILD` and other firmware-only shims are bootstrap aids, not
the final hardware contract. New peripheral work should therefore prefer a
QEMU/board-device implementation that makes production firmware observe the
same register/GPIO/bus behavior it sees on real hardware.
