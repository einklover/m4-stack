# Stage 9 — ESP32-S3 general-purpose SPI2 controller

Branch: `agent/m4-emulator-stage9-gpspi2`

Parent stage: `agent/m4-emulator-stage8-murphy-machine-touch`

Date: 2026-08-11

## Goal

Provide the firmware-visible ESP32-S3 GP-SPI2 master required by Murphy's e-paper
stack. This stage deliberately stops at the SoC SPI bus; the SSD1677 slave and
its CS/DC/RST/BUSY board wiring are Stage 10.

## Why an existing QEMU SPI model cannot simply be reused

The source audit found two nearby but incompatible controllers:

- `hw/ssi/esp32_spi.c` models the original ESP32 GP-SPI register layout;
- `hw/ssi/esp32s3_spi.c` models the ESP32-S3 SPI0/1 `SPI_MEM_*` flash/PSRAM path.

ESP32-S3 GP-SPI2 uses the generated S3 register contract beginning with:

- `CMD +0x00`
- `ADDR +0x04`
- `CTRL +0x08`
- `CLOCK +0x0c`
- `USER +0x10`
- `USER1 +0x14`
- `USER2 +0x18`
- `MS_DLEN +0x1c`
- interrupt block at `+0x34..+0x44`
- CPU data buffer `W0` at `+0x98` (16 words).

The relevant transaction controls are also S3-specific: CMD `USR` bit 24,
`UPDATE` bit 23; USER command/address/dummy/MISO/MOSI phase enables; command and
address bit lengths in USER2/USER1; and `TRANS_DONE` interrupt bit 12.

## Patch 0007

`0007-esp32s3-gpspi2.py` adds a dedicated `ssi.esp32s3.gpspi` SysBus device and
wires one instance as SPI2:

- MMIO at `DR_REG_SPI2_BASE`;
- IRQ to `ETS_SPI2_INTR_SOURCE` through the S3 interrupt matrix;
- QEMU SSI bus named `spi` for a real slave device;
- CPU W0 data-buffer transfers up to 64 bytes;
- command/address/dummy phases;
- MOSI/MISO phase handling;
- synchronous UPDATE completion;
- USER transaction completion and TRANS_DONE interrupt;
- interrupt enable/raw/status/clear/set behavior;
- reset and S3 peripheral-reset integration.

This is intentionally a **functional CPU transaction model**, not yet a full
cycle/timing/DMA implementation.

## Known remaining GP-SPI work

The first model does not yet implement:

- GDMA descriptor transfers;
- every clock/polarity/bit-order timing behavior;
- CS keep-active/multi-device timing;
- quad/octal data modes for GP-SPI;
- all error/interrupt bits.

For Murphy display bring-up, the next runtime question is whether the FreeInk
SPI path sends the 48,000-byte e-paper frame using CPU chunked transactions or
ESP32-S3 GDMA. If it uses DMA, Stage 10 acceptance will expose that boundary and
Stage 9 will be extended with the normal S3 GDMA descriptor path rather than a
special SSD1677 shortcut.

## Acceptance

Compile gate:

1. the full ordered QEMU patch series applies to the pinned source;
2. the new GP-SPI source is included in `hw/ssi/meson.build`;
3. `xtensa-softmmu` builds;
4. production-runner/patchset unit tests pass.

Runtime bus gate:

1. an ESP-IDF SPI2 CPU transaction writes normal S3 registers at
   `DR_REG_SPI2_BASE`;
2. writing CMD.USR emits the configured phases on the QEMU SSI bus;
3. CMD.USR self-clears after completion;
4. TRANS_DONE appears and can interrupt through `ETS_SPI2_INTR_SOURCE`.

Stage 10 then attaches SSD1677 to this SSI bus and validates the unchanged
production display path end-to-end.
