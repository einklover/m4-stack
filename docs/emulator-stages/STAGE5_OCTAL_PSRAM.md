# Stage 5 — production Octal/OPI PSRAM correctness

Branch: `agent/m4-emulator-stage5-octal-psram`

Parent stage: `agent/m4-emulator-stage4-qemu-adc` @
`ca4c64577b8649ed26f3f26f6de43b0bb156935c`

Date: 2026-08-11

## Goal

Remove the need to rebuild Murphy as Quad/QSPI PSRAM. The acceptance target is
the shipping N16R8/Octal production flash using `is_octal=true` with no guest
PSRAM compatibility profile.

## Source audit

The pinned Espressif QEMU already contains a substantial `ssi_psram` Octal
model. It supports the same command family used by ESP-IDF v5.5:

| Operation | ESP-IDF | QEMU |
|---|---:|---:|
| mode-register read | `0x4040` | `OCT_READ_REG = 0x4040` |
| mode-register write | `0xc0c0` | `OCT_WRITE_REG = 0xc0c0` |
| synchronous read | `0x0000` | `OCT_READ_SYNC = 0x0000` |
| synchronous write | `0x8080` | `OCT_WRITE_SYNC = 0x8080` |

For an 8 MiB device, QEMU returns the MR2 density code expected by ESP-IDF.
The IDF initialization path uses 16-bit commands and 32-bit addresses. It reads
mode registers, writes MR0, prints the device identity, then runs MSPI timing
tuning before configuring the SPI0 cache-side PSRAM phases.

The QEMU cache model directly maps PSRAM's RAM MemoryRegion through EXTMEM/MMU,
so not every physical SPI0 timing register needs analog/line-level behavior for
functional binary execution. Manual SPI1 transactions during initialization and
tuning, however, must be correct.

## Concrete upstream defect found

`hw/ssi/esp32s3_spi.c::esp32s3_spi_txrx_buffer()` iterates with `i`, but its
buffer guards incorrectly use the data byte variable:

```text
if (byte < tx_bytes) ...
if (byte < rx_bytes) ...
```

The TX condition is evaluated while `byte` is still zero, which can copy beyond
a shorter TX payload when RX is longer. The RX condition is evaluated after the
SPI transfer and can skip a response based on the transmitted byte value.

Patch `0002-esp32s3-spi-transfer-index.diff` changes both guards to the transfer
index. This is generic ESP32-S3 QEMU behavior and should be upstreamable.

## CI compile gate added

`.github/workflows/m4-qemu-patchset.yml` now:

1. checks out this stage;
2. installs QEMU build dependencies;
3. runs the production-runner Python tests;
4. clones the exact QEMU SHA from `patches/upstream.json`;
5. runs `git apply --check` for every patch in `series`;
6. builds `xtensa-softmmu`;
7. smoke-checks `qemu-system-xtensa --version`;
8. saves the QEMU source SHA/status as an artifact.

This converts the patchset from an uncompiled source proposal into a repeatable
remote build gate.

## What this stage does not claim yet

Patch 0002 is a confirmed correctness bug, but source inspection alone cannot
prove it is the only cause of the production Octal boot hang. The full N16R8
acceptance still needs a production flash/factory image accessible to the CI or
Mac QEMU host. A private production image should not be committed to a public
repository merely to satisfy CI.

## Next diagnostic if Octal still stops

Run the patched QEMU with SPI/cache logging around:

1. `s_init_psram_mode_reg()` and `s_get_psram_mode_reg()`;
2. `mspi_timing_psram_tuning()` reference writes/reads (`0xa5ff005a` pattern);
3. the transition into high-speed mode;
4. EXTMEM MMU entries tagged PSRAM;
5. the first PSRAM heap/cache accesses after app startup.

If manual SPI1 OPI transactions pass but the first mapped PSRAM access fails,
the next patch belongs in EXTMEM/MMU/cache rather than `ssi_psram`.

## Production acceptance command

```bash
python3 simulator/qemu/build_patched_qemu.py
export QEMU_XTENSA="$HOME/.cache/murphy-m4/espressif-qemu/build-murphy/qemu-system-xtensa"

python3 simulator/qemu/run_production_bin.py \
  /path/to/murphy-factory-16m.bin \
  --sd-image /path/to/card.img \
  --serial-file /tmp/murphy-production-octal.serial.log \
  --log /tmp/murphy-production-octal.qemu.log \
  --probe
```

Success for this gate means the unchanged production image passes PSRAM
initialization without the `murphy_m4_qemu` Quad profile. The next visible
failure is then allowed to move to the Murphy board peripherals.
