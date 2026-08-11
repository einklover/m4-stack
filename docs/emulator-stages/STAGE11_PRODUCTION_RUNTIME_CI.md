# Stage 11 — unchanged production-profile runtime gate

Branch: `agent/m4-emulator-stage11-production-runtime-ci`

Parent: cumulative integration v2 after Stage 8.

Date: 2026-08-11

## Goal

Turn binary compatibility from a source-level promise into an automated runtime
check. This workflow builds the **normal `murphy_m4` production profile**, not
`murphy_m4_qemu`, records its SHA-256, composes a 16 MiB flash image and boots
that image in the patched QEMU `murphy-m4` machine.

The guest therefore keeps production Octal/OPI PSRAM and contains no
`M4_QEMU_BUILD` compatibility path, ADC linker wrap or Quad-PSRAM substitution.

## Workflow

`.github/workflows/m4-production-bin-runtime.yml` performs:

1. install PlatformIO and QEMU build dependencies;
2. `scripts/bootstrap_deps.sh`;
3. `pio run -e murphy_m4`;
4. save SHA-256 of `.pio/build/murphy_m4/firmware.bin`;
5. compose an exact 16 MiB flash with `murphy_flash_image.py`;
6. create a raw FAT32 SD fixture;
7. apply/build the append-only QEMU v2 patch series;
8. run `run_production_bin.py` with Octal PSRAM and the Murphy machine;
9. classify the serial log with `probe_boot.py`;
10. require at least `application_reached` for this stage;
11. upload serial/QEMU/probe/hash provenance;
12. post the probe and serial tail directly to the PR on failure.

## What this proves

A green run proves that the exact production-profile application binary produced
in the same CI run reaches application startup through the real ESP32-S3 ROM,
second-stage bootloader and patched hardware model without a QEMU-specific guest
rebuild.

It is stronger than the old Quad `murphy_m4_qemu` smoke test. It is still not a
claim that a specific shipping unit's private NVS/eFuse/factory state has been
reproduced; a real 16 MiB factory dump can later replace the synthetic full-flash
baseline without changing the runner.

## Progressive acceptance

Stage 11 intentionally fails only if the production image cannot reach the app.
As board models mature, later stages tighten the gate in order:

- PSRAM initialized;
- SD ready;
- display ready;
- touch configured;
- boot summary;
- Home/reader;
- SSD1677 controller frame exported;
- network request succeeds.

This keeps every newly exposed hardware boundary measurable instead of hiding it
behind guest-side stubs.
