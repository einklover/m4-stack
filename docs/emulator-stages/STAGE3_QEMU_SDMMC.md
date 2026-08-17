# Stage 3 — native Espressif QEMU SDMMC attachment

Branch: `agent/m4-emulator-stage3-qemu-sdmmc`

Parent stage: `agent/m4-emulator-stage2-hardware-contract-sd` @
`24c6565d7a70701c930bca376fc40691348a625a`

Date: 2026-08-11

## Goal

Test the existing Espressif ESP32-S3 SDMMC implementation before writing any
Murphy-specific controller replacement.

## Upstream source finding

Current Espressif QEMU `hw/xtensa/esp32s3.c` already:

1. contains a `DWCSDMMCState` in the ESP32-S3 SoC;
2. realizes it at `DR_REG_SDMMC_BASE`;
3. connects its interrupt to `ETS_SDIO_HOST_INTR_SOURCE`;
4. calls `drive_get(IF_SD, 0, 0)` during machine initialization;
5. creates a `TYPE_SD_CARD` and realizes it on the controller's `sd-bus`.

The controller implementation is `hw/sd/dwc_sdmmc.c`. It is simplified but
supports command dispatch and DMA-mode transfers; FIFO/error/boot-mode details
are explicitly incomplete upstream.

This changes the diagnosis of the previous Murphy stop point: "SDMMC timeout"
does not prove the controller is absent. The first missing input was that the
Murphy QEMU runner did not attach an `if=sd` card image at all.

## Changes

### Production runner

`run_production_bin.py` now accepts:

```text
--sd-image /path/to/card.img
--sd-read-only
```

and emits the native QEMU drive:

```text
-drive file=/path/to/card.img,if=sd,format=raw
```

No guest firmware shim is involved.

### Fixture helper

Added `qemu/make_sd_image.py` to create an empty FAT32 raw image using the host's
`mkfs.fat`/`mkfs.vfat` or macOS `newfs_msdos`. A real card dump remains the
preferred reproduction fixture.

### Tests

The production-runner command tests now assert:

- native `if=sd` attachment;
- read-only mode;
- 512-byte sector-size validation.

## First experiment to run on the Mac QEMU host

For the QEMU-compatible guest (to isolate SD from the production Octal gate):

```bash
python3 simulator/qemu/make_sd_image.py /tmp/m4-sd.img --size-mb 64

# Add the same -drive manually to run_murphy_bin first if needed, or use the
# production runner once the Octal path reaches the app.
python3 simulator/qemu/run_production_bin.py \
  /path/to/murphy-factory-16m.bin \
  --sd-image /tmp/m4-sd.img \
  --serial-file /tmp/m4-prod-sd.serial.log \
  --probe
```

If the QEMU-only Quad guest reaches its old SD timeout even with `if=sd`, the
next step is to classify the DWC register/DMA behavior against the ESP-IDF S3
host driver rather than inventing a card protocol from scratch.

## Remaining board-level SD fidelity

The upstream controller/card attachment does not model the Murphy board's
switched `SD_PWR` rail or schematic `SD_CD` net. Those are board-device concerns
for later hotplug/power fidelity. For an initial boot-to-Home path, an attached,
powered card is an acceptable default because production firmware only starts
SD traffic after enabling the rail.

## Validation caveat

The current automation container has no Espressif QEMU binary/local GitHub clone,
so the source changes and tests are committed but the real boot experiment must
run on the Mac/CI host where `qemu-system-xtensa` is installed. This stage does
not claim that the unchanged production image has passed SD initialization yet.
