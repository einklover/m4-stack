# Murphy M4 firmware-agent contract

This repository is the validation harness for Murphy M4 firmware work. Agents
(Codex, Claude Code, OpenCode, human contributors) should use the lowest layer
that can truthfully reproduce a change, then climb the stack. Do not replace
real firmware logic with simulator-only logic merely to make a test pass.

## Preferred AI entrypoint

Use the graded, machine-readable wrapper before assembling commands manually:

```bash
python3 tools/ai_debug.py --list --json
python3 tools/ai_debug.py 0
python3 tools/ai_debug.py 2 --scenario slow_sd_tap --seeds 1:1000
python3 tools/ai_debug.py 4
```

Read `build/ai-debug/summary.json` as the authoritative result. Level 0-4 means
contracts, deterministic model, scheduler fuzz, QEMU boot, and QEMU screen.
Use `--through` only for a cumulative gate. This interface never touches a
physical device, `m4adb`, or flash. Full contract: `docs/AI_DEBUG_INTERFACE.md`.

## Hardware truth

The executable board contract is `board/murphy_m4.json` / `board/MurphyM4Spec.h`.
Its source priority is:

1. production-unit-probed FreeInk `BoardConfig::MURPHY_M4` values;
2. the supplied `SCH_ESP..18` schematic for physical topology;
3. field traces/issues for timing and resource calibration;
4. explicitly tagged inference only.

Never invent a GPIO or silently resolve a source discrepancy. In particular,
the schematic W25Q32 marking and the current N16R8/16MiB production firmware
layout disagree; the repository intentionally records both facts.

## Required layer selection

Before changing code, classify the failure:

| Failure class | First validation layer |
|---|---|
| Lua/native plugin scene, pagination, provider parsing | plugin/m4-device host simulator |
| page intent, EPD busy race, progressive index, frame provenance | deterministic `m4_simulator` |
| allocation placement, fragmentation, TLS/chapter residency | `m4_issue_contracts` + heap scenarios |
| GPIO/power/I2C/SPI/SSD1677/SDMMC protocol | `m4_board_tests` |
| Xtensa, FreeRTOS, real heap_caps, boot/partition, compiled UB | ESP32-S3 QEMU full-flash path |
| real EPD ghosting/LUT appearance, RF, signal integrity, battery chemistry | physical Murphy M4 |

Do not claim a higher-fidelity reproduction when only a lower-fidelity contract
was tested. `issues/regressions.json` enforces this vocabulary.

## Mandatory fast gate

From this repository:

```bash
python3 tools/validate_board_spec.py
python3 tools/validate_issue_matrix.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/m4_simulator
./build/m4_board_tests
./build/m4_issue_contracts
ctest --test-dir build --output-on-failure
python3 -m unittest discover -s tests -p 'test_*.py' -v
```

For memory/controller changes also run:

```bash
cmake -S . -B build-sanitize \
  -DM4SIM_SANITIZE=ON -DM4SIM_BUILD_NATIVE_SMOKE=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build-sanitize -j --target m4_board_tests m4_issue_contracts
./build-sanitize/m4_board_tests
./build-sanitize/m4_issue_contracts
```

## Deterministic race/fault work

Run one scenario with a complete timeline:

```bash
./build/m4_simulator --seed 1 bug_divergence -v
./build/m4_simulator --seed 1 tls_fragmented_heap -v
```

Then fuzz scheduler order without changing the fault model:

```bash
./build/m4_simulator --seeds 1:1000
```

A bug fix is incomplete when it only passes one lucky seed.

## Real firmware compile

In `einklover/m4-firmware`, the Murphy target is:

```bash
pio run -e murphy_m4
```

The current application slot is APP1 at `0x6e0000`; do not use a generic
full-flash command on a physical device. The simulator's QEMU composer is
separate and intentionally builds a disposable 16MiB image.

## Real binary in QEMU

Given `.pio/build/murphy_m4/{bootloader.bin,partitions.bin,firmware.bin}`:

```bash
python3 tools/murphy_flash_image.py \
  --build-dir ../m4-firmware/.pio/build/murphy_m4 \
  -o /tmp/murphy-qemu.bin

# after ESP-IDF export and qemu-xtensa installation
python3 qemu/run_full_flash.py /tmp/murphy-qemu.bin
```

Capture output and classify the first unsupported/failing boundary:

```bash
python3 qemu/probe_boot.py qemu.log -o qemu-probe.json
```

Never stub past a failure blindly. If the firmware stops at a board peripheral,
add/model that boundary with an explicit test, then rerun the same unmodified
firmware image.

## Physical trace feedback

Normalize serial/SD logs:

```bash
python3 tools/import_device_trace.py device.log -o device-trace.json
```

Use the trace to update `profiles/murphy_m4.json` with source attribution.
Do not turn one measured free-heap or EPD duration into a universal hardware
constant.

## Issue completion rule

For every fixed field bug:

1. add/update an entry in `issues/regressions.json` or
   `issues/plugin_regressions.json`;
2. identify the lowest truthful reproduction layer;
3. add a failing regression before/with the fix when feasible;
4. run the mandatory fast gate;
5. compile `murphy_m4` if firmware changed;
6. run QEMU when CPU/FreeRTOS/boot/resource behavior is involved;
7. require a physical-device check for analog/RF/display-appearance claims.

`reproduced` is reserved for failures with a local executable test reference.
If device evidence is missing, use `modeled` or `device_trace_required`.

## Architectural invariants

- One page-turn decision engine; backend-specific code must not duplicate it.
- `current target`, rendered-frame provenance and physical EPD commit are
  separate states.
- A valid page-turn intent eventually lands unless superseded.
- EPD MASTER_ACTIVATION while BUSY is a protocol violation.
- framebuffer and large decode/body data belong in PSRAM; TLS/HTTP control
  structures and task stacks that require internal memory must remain internal.
- SD publication is transactional: old generation remains authoritative until
  replacement sync/size verification completes.
- Empty/partial chapter paths never enter `TxtReaderActivity`.
- Do not conflate physical schematic presence with firmware-enabled capability.

See `docs/CODEX_FIRMWARE_DEBUG_GUIDE.md` for the full workflow and examples.
