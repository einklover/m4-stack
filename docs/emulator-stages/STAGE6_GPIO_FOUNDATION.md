# Stage 6 — functional ESP32-S3 GPIO foundation

Branch: `agent/m4-emulator-stage6-gpio-foundation`

Parent stage: `agent/m4-emulator-stage5-octal-psram`

Date: 2026-08-11

## Goal

Provide the digital GPIO substrate required by Murphy board devices. Without
this layer, patchless firmware cannot correctly observe keys/touch IRQ and its
outputs cannot actually control SD power, touch power, frontlight GPIO or panel
control pins.

## Upstream source finding

The inspected Espressif QEMU ESP32-S3 GPIO path was much thinner than expected:

- `hw/gpio/esp32s3_gpio.c` only subclasses the generic ESP32 GPIO device and
  supplies an S3 strap default;
- `hw/gpio/esp32_gpio.c` reads `GPIO_STRAP` but otherwise returns zero and
  ignores ordinary GPIO writes;
- the ESP32-S3 machine realizes the GPIO MMIO block but does not connect its IRQ
  to `ETS_GPIO_INTR_SOURCE`.

This exactly explains why the earlier QEMU guest could interpret floating/zero
inputs as a continuously held active-low power key.

## Patch 0003

`0003-esp32-gpio-digital-io.diff` adds the firmware-visible digital contract:

- OUT / OUT1 and W1TS/W1TC aliases;
- ENABLE / ENABLE1 and W1TS/W1TC aliases;
- IN / IN1 pad-state readback;
- STATUS / STATUS1 interrupt status and clear/set aliases;
- per-pin configuration register storage;
- edge/level interrupt generation from GPIO input changes;
- named QEMU `gpio-in[64]` and `gpio-out[64]` lines;
- a 64-bit `input-default` QOM property;
- reset behavior;
- ESP32-S3 GPIO IRQ connection into the interrupt matrix.

The model deliberately stays digital. IO_MUX routing, drive strength and analog
pad behavior remain later fidelity work unless production firmware proves they
are needed for a boot/function boundary.

## Murphy idle inputs

`run_production_bin.py` now defaults the external GPIO input mask to:

```text
0x100000000007
```

which holds GPIO0/1/2 high (active-low Power/Up/Down released) and GPIO44 high
(touch IRQ idle). This is supplied to the **QEMU GPIO device**, not to guest
firmware code.

The mask is host-overridable with `--gpio-input-default`, so a future Murphy
board device or monitor command can replace static defaults with real button
injection.

## Why this stage precedes touch/display

A Murphy SSD1677/touch model needs electrical-looking endpoints:

- SSD1677: CS/DC/RST/BUSY plus SPI;
- touch: power + IRQ plus I2C;
- SD: switched power + card detect around the existing SDMMC controller;
- keys: external active-low GPIO inputs.

Stage 6 creates the general QEMU pin boundary for these connections. It avoids
implementing each peripheral as a private host shortcut that bypasses ESP-IDF
GPIO code.

## Patch application robustness

The earlier ADC patch exposed a tooling issue: manually authored unified-diff
hunk counts can become syntactically corrupt before C compilation is reached.
The patch builder now supports context-checked `.py` source transforms for
larger SoC modifications and ordinary `.diff` entries for small edits. Every
entry runs `git diff --check`; the full patched source is then compiled by the
GitHub Actions QEMU gate.

## Acceptance

Minimum Stage 6 acceptance on patched QEMU:

1. patch series applies and Xtensa QEMU builds in Actions;
2. production runner no longer needs the firmware `loop()` workaround merely to
   avoid a false held power key;
3. GPIO0/1/2 can be driven as host inputs and are visible through GPIO IN;
4. GPIO10 output/enable transitions are observable for Murphy `SD_PWR`;
5. configured GPIO interrupt input changes reach the S3 interrupt matrix.

Actual interactive buttons and touch events are the next board-device stage.
