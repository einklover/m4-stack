# Stage 1 — patchless production-boot baseline

Branch: `agent/m4-emulator-stage1-production-boot`

Date: 2026-08-11

## Goal

Create a permanent acceptance path where the guest is the same production
Murphy M4 flash/firmware used on hardware.  Emulator deficiencies must be fixed
below the guest rather than hidden by `M4_QEMU_BUILD` conditionals.

## Changes

1. Added `simulator/qemu/run_production_bin.py`.
   - requires an exact 16 MiB full flash image;
   - requests ESP32-S3 + 8 MiB Octal PSRAM;
   - keeps watchdog behavior unless `--disable-wdt` is explicitly requested;
   - supports QEMU ESP32-S3 joint-format eFuse backing files;
   - makes `open_eth` opt-in and labels it diagnostic-only;
   - supports serial capture, GDB, boot probing and extra raw QEMU flags.
2. Added command-contract tests under `simulator/tests/`.
3. Established `hardware/` as the canonical board knowledge base and extracted
   the newly supplied 2026-05-06 schematic facts.

## Acceptance command

```bash
python3 simulator/qemu/run_production_bin.py \
  /path/to/murphy-factory-16m.bin \
  --efuse-file /path/to/qemu_efuse.bin \
  --serial-file /tmp/murphy-production.serial.log \
  --probe
```

For a device that does not require copied eFuses, omit `--efuse-file`.

## Current expected boundary

This stage intentionally does **not** claim a successful Home boot on stock
Espressif QEMU.  The previously proven QEMU-only guest reaches `setup()` only
because it changes the production Octal-PSRAM profile and bypasses unmodeled
hardware.  The new production runner preserves those failures as testable
boundaries.

The next implementation stages therefore target:

1. ESP32-S3 startup/ADC behavior without linker wrapping;
2. production Octal PSRAM/MSPI behavior;
3. SDMMC + powered/removable `sdcard.img` based on the supplied schematic;
4. GPIO/buttons/touch;
5. SSD1677/panel SPI + BUSY;
6. power/frontlight/sensors;
7. functional networking and reset/sleep fidelity.

## Hardware evidence discovered during this stage

The supplied schematic is `ESP32_426_S3_V2.0`, MCU `ESP32-S3R8`.  It explicitly
shows 4-bit SDMMC nets, card detect and switched SD power.  It also exposes
panel SPI/BUSY, touch I2C/IRQ/power, dual-channel frontlight PWM, battery ADC,
SC7A20 and AHT20.

A material conflict is preserved in `hardware/murphy-m4/FACTS.md`: schematic U4
is W25Q32 (4 MiB) while the shipping firmware profile/partition map uses 16 MiB
flash.  A real-device JEDEC-ID read is required before freezing the flash chip
model.

## Validation policy

This branch is a baseline, not a compatibility success claim.  A future stage
may only call the binary emulator complete when the SHA-256-identical production
firmware/flash boots without guest patches and reaches the same Home/reader path
with board I/O supplied by emulator devices.
