# M4Sim — Murphy M4 firmware validation harness

M4Sim is a layered development/debugging harness for the Murphy M4
(ESP32-S3R8/N16R8 + SSD1677 800×480 e-ink). It combines deterministic race and
fault simulation, schematic-grounded board/controller models, real-firmware
ESP32-S3 QEMU execution and cross-repository plugin host regressions.

The goal is not to make one visually convincing mock. The goal is to catch the
lowest-level truthful failure before a physical flash:

```text
edit firmware/plugin
      |
      +-- app/plugin host regressions
      +-- deterministic race/fault suite (many seeds)
      +-- heap/TLS/storage/lifetime contracts
      +-- GPIO/I2C/SPI/SDMMC/SSD1677 board model
      +-- actual Murphy firmware.bin in ESP32-S3 QEMU
      |
      `-- physical M4 only for final analog/RF/electrical acceptance
```

See `AGENTS.md` for agent rules and `docs/CODEX_FIRMWARE_DEBUG_GUIDE.md` for the
full workflow.

## AI quick start

Use one graded command instead of remembering the underlying toolchain:

```bash
python3 tools/ai_debug.py --list --json
python3 tools/ai_debug.py 0          # contracts/tooling
python3 tools/ai_debug.py 1          # deterministic C++/board model
python3 tools/ai_debug.py 2          # scheduler fuzz (1:200 by default)
python3 tools/ai_debug.py 3          # real firmware boot in QEMU
python3 tools/ai_debug.py 4          # QEMU boot + 480x800 PBM screen
```

Add `--through` to run every lower level, or `--json` for machine-only stdout.
The authoritative report is `build/ai-debug/summary.json`; every subprocess has
its own log there. See `docs/AI_DEBUG_INTERFACE.md` for the stable JSON contract,
failure protocol, artifacts and fidelity limits. The wrapper never touches a
physical device or `m4adb`.

## Architecture

```text
plugin/native provider logic      m4-device/plugin host simulator
             |
             v
core/PageTurnCoordinator.h        shared reader decision engine
             |
       model/ReaderModel
             |
     DisplayPort / StoragePort
       |                |
   SimPanel         SimStorage       deterministic behavior/fault layer
             |
────────────────────────────────────────────────────────────
board/MurphyM4Spec.h + MurphyBoard  schematic/live-probe contract
SimGpio / SimPower / SimSpiBus / SimI2cBus / SimSdmmc
SimSsd1677Controller                register/RAM/BUSY/commit layer
────────────────────────────────────────────────────────────
             |
 real 16MiB PlatformIO flash image
             |
 Espressif ESP32-S3 QEMU            Xtensa/FreeRTOS/boot/heap/MMIO
             |
 physical Murphy M4                 EPD analog/RF/power/signal integrity
```

Detailed dependency/fidelity rules: `docs/ARCHITECTURE.md`.

## Hardware contract

`board/murphy_m4.json` and `board/MurphyM4Spec.h` are the machine-readable
Murphy board profile. The current executable pinout is based on the FreeInk
Murphy profile whose values were independently exercised on a production unit;
the supplied schematic remains physical-topology evidence.

Current profile:

```text
EPD SSD1677: SCLK4 MOSI3 CS5 DC6 RST7 BUSY8, 10MHz
SDMMC 4-bit: PWR10 active-low, CLK16 CMD15 D0=17 D1=18 D2=11 D3=14
keys:        UP1 DOWN2 LOCK/POWER0 active-low
battery:     ADC9
I2C0:        SDA13 SCL12
Touch:       IRQ44 PWR45 active-low, address 0x2e
buzzer:      GPIO46
frontlight:  warm47 cool48
```

The schematic physically contains AHT20/SC7A20, while current firmware exposes
`NO_SENSORS`; the simulator deliberately distinguishes physical presence from
firmware capability.

A known source discrepancy is preserved rather than guessed away: the schematic
marks W25Q32 (nominal 4MiB), while current production firmware/factory layout is
N16R8/16MiB. The executable emulator uses 16MiB until a production JEDEC trace
resolves the populated flash part.

Validate the contract:

```bash
python3 tools/validate_board_spec.py
```

## Fast validation gate

```bash
python3 tools/validate_board_spec.py
python3 tools/validate_issue_matrix.py
python3 -m json.tool profiles/murphy_m4.json >/dev/null

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/m4_simulator
./build/m4_board_tests
./build/m4_issue_contracts
./build/m4_native_smoke
ctest --test-dir build --output-on-failure
python3 -m unittest discover -s tests -p 'test_*.py' -v
```

CI also runs ASan/UBSan on the low-level board and issue-contract tests.

## Deterministic scenarios

```text
single_tap_slow_index   one unindexed intent eventually lands
bug_lost_tap            regression knob: BUSY tap is lost and assertion catches it
rapid_tap_correct       rapid taps converge to final target
bug_divergence          regression knob: live target used as physical provenance
                        and invariant catches it
tls_fragmented_heap     total/PSRAM free but no 48KiB internal contiguous block
 tls_keepalive           handshake peak vs connection-resident memory
framebuffer_contract    internal framebuffer placement rejected; PSRAM required
chunked_quiet_eof       arbitrary TCP/chunk segmentation survives long quiet gaps
slow_sd_tap             SD latency cannot erase user intent
heap_stress             allocation/realloc/free invariants
heap_fuzz               independent shadow allocator model vs SimHeap
```

Run one with a timeline:

```bash
./build/m4_simulator --seed 1 bug_divergence -v
```

Schedule fuzz:

```bash
./build/m4_simulator --seeds 1:1000
```

## Low-level board/controller model

`m4_board_tests` covers:

- GPIO ownership and electrical contention;
- active-low SD/touch power gates;
- shared I2C topology;
- firmware-view vs physical-view devices;
- native 4-bit SDMMC mount/power-loss/in-flight I/O failure;
- SPI transaction/clock discipline;
- SSD1677 BW/RED RAM, ranges/counters, update controls, LUT upload,
  MASTER_ACTIVATION, active-high BUSY and atomic physical commit;
- rejection of a second activation while BUSY.

The SSD1677 model is digital/controller fidelity. Analog ghosting, waveform gray
appearance and temperature chemistry remain physical-device acceptance items.

## Firmware issue contracts

`issues/regressions.json` is machine-validated. A row may say `reproduced` only
when it names a local executable scenario/CTest.

Current covered classes include:

- #2 fragmented internal RAM / TLS OOM;
- #4 reader/TLS residency tradeoff;
- #5 empty-path and repeated cross-chapter reader-generation handoff;
- #9 TLS worker/control-plane internal residency vs PSRAM payloads;
- #24 transactional SD publication with rename failure, bounded 2KiB fallback,
  sync/size verification and preservation of the old generation;
- #1 SSD1677 controller semantics, with analog appearance correctly left as a
  real-device calibration requirement.

Cross-repository Fanqie/JJWXC/WeRead regressions are tracked in
`issues/plugin_regressions.json` and remain owned by the real m4-device/plugin
host tests for business logic.

## Field calibration and trace import

`profiles/murphy_m4.json` contains source-attributed observations (heap minima,
TLS defaults, EPD timing measurements, issue-specific reader snapshots).

Normalize a real serial/SD log:

```bash
python3 tools/import_device_trace.py device.log -o device-trace.json
```

Page-turn shadow mismatches (`[PTSH]`), heap samples, reader opens, panic markers
and EPD TP timings are extracted without discarding unclassified evidence.

## Real Murphy firmware binary in ESP32-S3 QEMU

The QEMU path is not a toy `app_main` only. It can compose and execute the actual
PlatformIO Murphy build.

Build firmware:

```bash
cd ../m4-firmware
pio run -e murphy_m4
```

Compose a disposable full 16MiB image:

```bash
cd ../murphy-m4-simulator
python3 tools/murphy_flash_image.py \
  --build-dir ../m4-firmware/.pio/build/murphy_m4 \
  -o /tmp/murphy-qemu.bin
```

Then, with ESP-IDF and Espressif `qemu-xtensa` installed/exported:

```bash
python3 qemu/run_full_flash.py /tmp/murphy-qemu.bin
```

Classify a captured QEMU log:

```bash
python3 qemu/probe_boot.py qemu.log -o qemu-probe.json
```

`.github/workflows/qemu-real-firmware.yml` automates the full chain:

```text
checkout m4-firmware Phase-A
 -> pio run -e murphy_m4
 -> compose exact 16MiB flash
 -> install ESP-IDF/qemu-xtensa
 -> boot the real image
 -> archive log
 -> classify first failing/unsupported board boundary
```

The rule is to model the next boundary and rerun the **same firmware image**, not
to patch production firmware so QEMU skips initialization.

## Full-flash layout

Current firmware/factory layout:

```text
0x008000  partition table
0x009000  NVS
0x00e000  OTA data
0x010000  APP0 / ota_0    size 0x6d0000
0x6e0000  APP1 / ota_1    size 0x6d0000
0xdb0000  SPIFFS
0xff0000  coredump
           total 16MiB
```

Blank-QEMU mode mirrors the byte-identical app into APP0 and APP1 so erased OTA
metadata boots deterministically. With a real 16MiB base dump, `--base-flash`
preserves factory/NVS/OTA/APP0/SPIFFS and overlays APP1 only.

## Cross-repository plugin acceptance

`.github/workflows/plugin-host-regressions.yml` runs focused `m4-device`
simulator targets for:

- Fanqie Lua journey + memory regression;
- JJWXC Lua journey;
- WeRead progress-shard round-trip;
- plugin reader lifecycle/audit;
- architecture fault injection.

This keeps provider/parser truth in the repository that actually executes the
Lua/native host while M4Sim owns the hardware/resource contracts.

## Production PageTurnCoordinator migration

The real firmware already has a Phase-A differential wiring branch/PR: legacy
`TxtReaderActivity` remains authoritative while the shared coordinator observes
tap, render snapshot, physical commit and catch-up decisions. Mismatches are
logged as `[PTSH] DIFF` and the full Murphy build passes.

Phase B should happen only after real-device shadow traces are clean: make the
coordinator authoritative and remove duplicate target/pending/physical/quick
state from `TxtReaderActivity`.

## Agent usage

For Codex/Claude/OpenCode, start with:

- `AGENTS.md`
- `docs/CODEX_FIRMWARE_DEBUG_GUIDE.md`
- `docs/ARCHITECTURE.md`
- `issues/regressions.json`
- `board/murphy_m4.json`

The key rule is simple: **use the lowest layer that can truthfully reproduce the
bug, then climb the stack.** Physical flashing should be the final hardware
acceptance step, not the first debugger.
