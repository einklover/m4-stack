# Stage 7 — wire the existing QEMU I2C master into ESP32-S3

Branch: `agent/m4-emulator-stage7-i2c-controller`

Parent stage: `agent/m4-emulator-stage6-gpio-foundation`

Date: 2026-08-11

## Goal

Provide the real ESP32-S3 I2C MMIO/interrupt/bus path required by the Murphy
touch controller and other board I2C devices. The production guest must use its
normal ESP-IDF/Arduino I2C driver rather than a firmware-side touch shortcut.

## Upstream finding

Espressif QEMU already ships `hw/i2c/esp32_i2c.c`. It implements:

- the master control/start path;
- RX/TX FIFOs;
- the 16 command registers;
- repeated-start/write/read/stop/end command execution;
- ACK error, transaction-complete and end-detect interrupts;
- a real QEMU `I2CBus` child where slave devices can be attached.

Its key register offsets match ESP32-S3's generated IDF register map (control at
`+0x04`, status at `+0x08`, FIFO config/data, interrupt registers and command
window). The S3 machine, however, did not instantiate this already-compiled
controller at all.

ESP-IDF's ESP32-S3 peripheral table confirms:

- I2C0 uses `ETS_I2C_EXT0_INTR_SOURCE`;
- I2C1 uses `ETS_I2C_EXT1_INTR_SOURCE`.

The S3 address map provides `DR_REG_I2C_EXT_BASE = 0x60013000` and
`DR_REG_I2C1_EXT_BASE = 0x60027000`.

## Patch 0004

`0004-esp32s3-i2c-controller.py` is a context-checked source transform that:

1. includes the existing `esp32_i2c.h` controller API;
2. adds two `Esp32I2CState` children to `Esp32s3SocState`;
3. initializes both as normal QOM children;
4. resets them on peripheral reset;
5. realizes their MMIO regions at the two S3 I2C bases;
6. connects their IRQs to the matching S3 interrupt-matrix sources.

No Murphy-specific touch logic is present in this stage. That is deliberate:
Stage 7 creates the SoC controller/bus first; Stage 8 can attach a Murphy touch
slave at address `0x2e` to I2C0 and control its power/IRQ through Stage 6 GPIO.

## Acceptance

The GitHub Actions QEMU gate must compile the full ordered patch series. A later
runtime smoke fixture should then run an ESP-IDF I2C master transaction through
`0x60013000`, receive a slave ACK, complete a command list and observe the I2C0
interrupt through the normal S3 interrupt matrix.

Production Murphy acceptance is stronger: the unchanged firmware's touch probe
must stop timing out once the FT6x36-compatible slave is attached in Stage 8.
