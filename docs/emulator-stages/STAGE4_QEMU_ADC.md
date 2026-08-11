# Stage 4 — remove the ADC startup guest bypass at the QEMU layer

Branch: `agent/m4-emulator-stage4-qemu-adc`

Parent stage: `agent/m4-emulator-stage3-qemu-sdmmc` @
`4cb116400ce9158443b839e0c66cba1b4dd62770`

Date: 2026-08-11

## Root cause, now pinned to registers

ESP-IDF v5.5 `adc_hal_self_calibration()` repeatedly calls the S3 oneshot path.
For ADC1/ADC2 that path:

1. clears the previous event;
2. toggles `meas*_start_sar`;
3. waits in `while (!adc_oneshot_ll_get_event(...))`;
4. reads `meas*_data_sar`.

The S3 low-level header maps this to `SENS_SAR_MEAS1_CTRL2` and
`SENS_SAR_MEAS2_CTRL2`. The generated register definitions establish:

- `DR_REG_SENS_BASE = 0x60008800`;
- ADC1 CTRL2 offset `0x0c`;
- ADC2 CTRL2 offset `0x30`;
- START = bit 17;
- DONE = bit 16;
- DATA = bits 15:0.

Espressif QEMU's ESP32-S3 machine currently leaves unsupported I/O in one broad
`esp32s3.iomem` region. Its fallback read handler returns zero and write handler
does nothing. SENS is not realized as a dedicated device in the inspected
upstream source, so `DONE` can never become one.

This explains the previously observed pre-`setup()` hang without relying on a
firmware-side guess.

## Changes

### Reproducible QEMU patch workspace

Added:

- `qemu/patches/upstream.json` — exact Espressif QEMU source SHA;
- `qemu/patches/series` — ordered patch list;
- `qemu/build_patched_qemu.py` — clone/reset, `git apply --check`, apply,
  configure and build `xtensa-softmmu`;
- `qemu/patches/README.md` — patch/rebase policy.

### Patch 0001

`0001-esp32s3-sens-adc-oneshot.patch` adds a minimal SENS register shadow to
`hw/xtensa/esp32s3.c` and handles ADC1/ADC2 START→DONE at the real MMIO boundary.
A conversion returns deterministic zero data, which is sufficient for internal-
ground self-calibration to terminate. It does not claim battery/analog accuracy.

The patch also passes the ESP32-S3 SoC object as the I/O-region opaque pointer
so the generic I/O callbacks have per-machine SENS state, and clears that state
on peripheral reset.

## Acceptance target

With the patched QEMU binary, use the **production** flash runner, not
`murphy_m4_qemu`:

```bash
python3 simulator/qemu/build_patched_qemu.py
export QEMU_XTENSA="$HOME/.cache/murphy-m4/espressif-qemu/build-murphy/qemu-system-xtensa"

python3 simulator/qemu/run_production_bin.py \
  /path/to/murphy-factory-16m.bin \
  --sd-image /path/to/card.img \
  --serial-file /tmp/murphy-production.serial.log \
  --probe
```

The acceptance criterion for this stage is that the production guest no longer
requires `-Wl,--wrap=adc_hal_self_calibration` to pass the ADC constructor.

## Validation caveat

The patch is targeted at the pinned upstream source and the builder performs
`git apply --check` before build. The current ChatGPT automation container cannot
clone/build Espressif QEMU, so this branch does not claim that `git apply`, QEMU
compilation or a real production boot was executed here. The Mac/CI host must
run the commands above before this patch is considered validated.

## Next gate

The largest remaining production-startup incompatibility is Octal/OPI PSRAM.
Unlike ADC, this touches the SPI1/MSPI/PSRAM/cache path and must be diagnosed
against upstream `ssi_psram`, SPI1 and cache behavior rather than replaced with
a Quad-PSRAM guest profile.
