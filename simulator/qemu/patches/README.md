# Murphy patches for Espressif QEMU

These patches exist to make **unmodified production Murphy M4 binaries** observe
missing ESP32-S3/board hardware behavior. They must not become a second guest
firmware compatibility layer.

## Reproducibility

`upstream.json` pins an exact `espressif/qemu` commit. `series` defines patch
order. Build with:

```bash
python3 simulator/qemu/build_patched_qemu.py
export QEMU_XTENSA="$HOME/.cache/murphy-m4/espressif-qemu/build-murphy/qemu-system-xtensa"
```

The repository workflow `.github/workflows/m4-qemu-patchset.yml` repeats the
patch/apply/build sequence on GitHub Actions, runs the production-runner unit
tests, smoke-checks the resulting `qemu-system-xtensa`, and uploads provenance.
This gives every patch stage a compile gate even when a local QEMU toolchain is
not available.

Before changing the upstream SHA:

1. rebase every patch;
2. run `git apply --check` through the builder;
3. rebuild QEMU;
4. rerun the production-flash acceptance command;
5. record the new boot checkpoint in the stage handoff.

## Patch 0001 — SENS ADC oneshot completion

ESP-IDF v5.5's S3 ADC self-calibration starts an RTC/SENS oneshot and polls the
SENS `meas*_done_sar` bit. Upstream QEMU currently leaves the SENS range in the
generic unsupported I/O window, whose reads return zero. That makes the
constructor wait forever before `app_main`/Arduino setup.

The first patch implements only the digital contract needed to stop that hang:
SENS register storage plus START→DONE and deterministic internal-ground data.
It deliberately does **not** claim analog ADC accuracy. Battery-voltage and
external-channel analog input belong to the Murphy board model and can be added
as QOM properties later.

## Patch 0008 — SSD1677 attach

Attaches a digital `murphy-ssd1677` SSI peripheral to GP-SPI2. Requires
`#include "hw/ssi/ssi.h"` in `esp32s3.c` so `SSI_BUS` expands as a QOM cast
macro (otherwise the cumulative series fails to link with an undefined
`SSI_BUS` reference).

## Patch 0009 — SPI0 alias onto SPI1

Production second-stage bootloaders and MSPI paths access SPI0 at
`0x60003000` as well as SPI1 at `0x60002000`. Upstream only realizes SPI1;
SPI0 falls into the catch-all iomem window and returns zero, which leaves the
guest spinning after `entry` while APP CPU waits in ROM `main` for
`ets_set_user_start`.

This patch aliases SPI0's MMIO onto the existing SPI1 controller so both
register windows drive the same flash/PSRAM SSI bus. With it applied, an
unmodified production `murphy_m4` image progresses past second-stage into the
Arduino/ESP-IDF application (visible via UART0 host logs even when USB-CDC
`Serial` is silent).

## Patch 0002 — SPI transfer-buffer indexing

The ESP32-S3 SPI helper iterates `i = 0 .. max(tx_bytes, rx_bytes)` but used the
current data byte as the bounds predicate for both buffers. For TX that can read
past a shorter TX buffer in asymmetric full-duplex transactions; for RX it can
skip a destination byte based on the transmitted byte value. Patch 0002 uses
`i < tx_bytes` and `i < rx_bytes` as intended.

This is a generic ESP32-S3 QEMU correctness fix, not a Murphy-specific bypass.
It matters during Octal-PSRAM investigation because SPI1 is used by ESP-IDF for
OPI mode-register transactions and MSPI timing-tuning reads/writes. It is a
necessary correctness fix, but by itself is **not** yet claimed to be the sole
cause of the production N16R8 boot hang.

## Octal PSRAM facts checked against ESP-IDF

The pinned QEMU PSRAM model already understands the same 16-bit AP Memory OPI
commands used by ESP-IDF (`0x4040`, `0xc0c0`, `0x0000`, `0x8080`) and exposes
8 MiB density through MR2. ESP-IDF v5.5 uses 32-bit addresses, 8 dummy bits for
mode-register reads, 18 dummy bits for synchronous reads and 8 dummy bits for
writes. The QEMU byte-level model's configured 1/3/1 dummy-byte expectations
match those transactions after SPI register cycle counts are rounded to bytes.

The remaining investigation must therefore distinguish:

- SPI1 manual OPI transaction correctness;
- MSPI timing-tuning loops;
- SPI0 cache-side configuration registers that upstream does not model in the
  same detail as SPI1;
- EXTMEM/MMU mapping of PSRAM pages;
- differences between the older installed QEMU 9.2.2 build and this pinned
  `esp-develop` source baseline.

## Patch 0010 — AES + GDMA (TLS)

mbedtls hardware AES hangs because GDMA could bind the wrong channel
(`peri_sel || START`), could not DMA to IRAM/PSRAM, and because QEMU modeled
I-bus SRAM and D-bus DRAM as two different RAM objects. Silicon uses one SRAM
window; GDMA reconstructs only a 20-bit DRAM address.

This patch matches channels by `peri_sel` only, points GDMA at system memory,
and aliases `esp32s3.iram` onto `esp32s3.dram`. After it, production-style
HTTPS (WeRead/JJWXC) should complete a handshake without firmware URL shims.

## Patch 0011 — GDMA full address + AES always DONE

ESP-IDF writes the full virtual descriptor address into the 32-bit LINK
register. Reconstructing only the 20-bit DRAM window mis-translates PSRAM
buffers used by larger TLS records (WeRead shelf). Combined with AES leaving
STATE unset on GDMA failure, mbedtls busy-waits and the guest looks frozen
after login.

Use the software high bits when present, and always retire AES DMA with
STATE=DONE so a failed descriptor cannot wedge UART/input.

## Rules for future patches

- Prefer an existing upstream QEMU device before adding a replacement. Stage 3
  demonstrated this for SDMMC.
- Keep SoC fixes generic ESP32-S3 behavior when possible; keep Murphy-specific
  GPIO/peripheral wiring in a board machine/device layer.
- Every bypass removed from `M4_QEMU_BUILD` should have a corresponding
  production-bin acceptance assertion before deleting the bootstrap path.
- Do not patch the guest binary or rewrite its hash to hide emulator failures.
