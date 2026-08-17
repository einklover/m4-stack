# Stage 8 — dedicated `murphy-m4` machine + touch device

Branch: `agent/m4-emulator-stage8-murphy-machine-touch`

Parent stage: `agent/m4-emulator-stage7-i2c-controller`

Date: 2026-08-11

## Goal

Separate generic ESP32-S3 SoC emulation from Murphy-specific board wiring. The
production runner should now boot `-machine murphy-m4`; board peripherals attach
to the normal SoC buses/GPIO lines below an unchanged guest.

## Machine split

Patch 0005 registers a `murphy-m4` QEMU machine whose parent is `esp32s3`.
It inherits the ESP32-S3 ROM/CPU/memory/peripheral setup and adds only hardware
physically present on the Murphy reader. The generic `esp32s3` machine remains
available for SoC-only diagnostics.

This becomes the attachment point for later SSD1677, SD power/card detect,
frontlight and power/battery devices without polluting Espressif's generic S3
machine.

## FT6x36-compatible touch

The first Murphy board device is a small I2C slave at address `0x2e`, matching
the existing deterministic `SimFt6x36Device` contract and current firmware:

- guest selects register 0;
- an 8-byte frame is returned;
- byte 2 contains point count;
- byte 3 contains contact event bits and X high nibble;
- bytes 4-6 contain X/Y;
- valid logical coordinates are 480x800;
- TOUCH_INT is idle high and asserted low for an active contact;
- GPIO45 drives the active-low touch power input;
- touch IRQ drives GPIO44 through the Stage 6 GPIO input line.

When unpowered, I2C start is NACKed. This is board-visible behavior rather than
a firmware test hook.

## Host fixture

`run_production_bin.py` defaults to:

```text
-machine murphy-m4
```

and adds:

```text
--touch X,Y
```

for a deterministic initial contact via QOM global properties. The generic
`--machine esp32s3` path is still selectable; a Murphy touch fixture is rejected
there because the board device does not exist.

This static fixture is intentionally only the first input mechanism. A later
host/QMP input layer can change touch state and key GPIO levels interactively
without changing the I2C frame or GPIO contracts established here.

## Patch implementation

- `0005-murphy-m4-machine-touch.py` creates the I2C slave source, adds it to the
  S3 build, registers the Murphy machine subtype, attaches it to I2C0 and wires
  GPIO44/45.
- `0006-murphy-touch-qdev-reset.py` adapts the device reset callback to the
  pinned QEMU 9.x `DeviceClass::legacy_reset` API.

Both are context-checked transforms and are compiled by the cumulative QEMU
patchset validation workflow.

## Acceptance

Compile acceptance:

1. full patch series applies to the pinned QEMU SHA;
2. `qemu-system-xtensa -machine help` contains `murphy-m4`;
3. production runner unit tests pass;
4. patched QEMU builds successfully.

Runtime acceptance with a private production image:

1. firmware I2C0 probe receives an ACK at `0x2e` when touch power is on;
2. GPIO45 high makes the touch device NACK and deassert IRQ;
3. `--touch 123,456` produces the same 8-byte frame semantics as the deterministic model;
4. GPIO44 interrupt reaches the unchanged production input path.
