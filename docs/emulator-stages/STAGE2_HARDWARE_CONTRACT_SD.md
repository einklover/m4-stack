# Stage 2 — shared hardware contract + raw SD backing

Branch: `agent/m4-emulator-stage2-hardware-contract-sd`

Parent stage: `agent/m4-emulator-stage1-production-boot` @
`c0bb4fa37eeffb9f20e36a564042c6263ce526dd`

Date: 2026-08-11

## Goal

Turn the existing deterministic board model into the reference contract for the
QEMU Murphy board implementation, and upgrade its SD model from capacity-only
timing to real 512-byte raw-sector storage.

## Changes

### Hardware knowledge base

- Added `hardware/murphy-m4/NETS.md` with the named schematic nets and the
  executable BoardConfig-derived GPIO contract.
- Added `hardware/murphy-m4/EMULATOR_MODEL.md` mapping every relevant device
  between deterministic M4Sim and the production-bin QEMU path.
- Pinned the supplied schematic source by exact size and SHA-256 while clearly
  recording that the current GitHub connector cannot upload the PDF binary.
- Kept `SD_CD` unresolved rather than guessing its GPIO number.

### Executable board contract

Added `m4_board_contract` (`simulator/tools/board_contract.cpp`). It emits JSON
directly from `MurphyM4Spec.h`, including:

- MCU/PSRAM/flash contract and the known 4 MiB-vs-16 MiB flash discrepancy;
- SSD1677 geometry/SPI/pins;
- 4-bit SDMMC pins/power polarity;
- keys, touch, battery, frontlight and physical I2C capability information;
- unresolved facts that must not silently turn into emulator assumptions.

### Raw SD-card backing

Added `SimSdCardImage`:

- raw 512-byte sectors;
- create/load/save;
- read-only or writable images;
- dirty tracking;
- no FAT abstraction (filesystem remains guest-owned).

Extended `SimSdmmc`:

- `insertImage()` and image-backed capacity;
- asynchronous sector payload reads;
- asynchronous sector writes;
- injected nth-read/nth-write failures;
- card-detect logical state;
- removal and rail-power generation invalidation;
- image flush support;
- existing boolean-only read API preserved for compatibility.

Added `m4_sd_image_tests` covering sector roundtrip and in-flight removal/power
races.

## Why this matters for production BIN execution

The schematic confirms that Murphy does not have an abstract always-on storage
service: it has 4-bit SDMMC plus `SD_PWR` and `SD_CD`. The QEMU implementation
therefore needs both an ESP32-S3 SDMMC controller path and board-level
power/presence wiring. Stage 2 now provides the reference storage semantics and
raw-image format that the QEMU device must match.

## Validation

The branch contains CMake test targets, but the current automation environment
cannot clone/build the GitHub repository locally (outbound GitHub DNS is
unavailable and `gh` is not installed). Therefore this stage records tests in
source but does **not** claim they were executed here. CI or a normal local clone
must run:

```bash
cd simulator
cmake -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/m4_board_contract
```

## Next stage

Build a reproducible Espressif-QEMU integration layer/patch workspace that takes
the production runner plus exported board contract and adds the first missing
SoC/board hardware behavior below the guest. The first targets are ADC startup
completion and SDMMC plumbing; Octal PSRAM remains the separate production-boot
gate that must also be fixed rather than bypassed.
