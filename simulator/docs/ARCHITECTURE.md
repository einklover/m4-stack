# M4Sim architecture

M4Sim is a **layered Murphy M4 firmware validation harness**, not one giant mock.
Each layer owns a different fidelity question and must not make claims belonging
to a higher-fidelity layer.

```text
                    plugin / native app behavior
                m4-device + plugin host simulators
                              │
                              ▼
                   shared reader decisions
                core/PageTurnCoordinator.h
                              │
                              ▼
                       model/ReaderModel
                              │
                 DisplayPort / StoragePort
                     │                 │
        ┌────────────┴───────┐   ┌────┴────────────┐
        │                    │   │                 │
 deterministic reader     native host        deterministic storage
 SimPanel/SimStorage       seam smoke         fault/timing model
        │
        │ frame intent / provenance
        ▼
────────────────────────────────────────────────────────────────────
                 Murphy board/controller layer
          board/MurphyM4Spec.h + MurphyBoard
 GPIO / power / SPI / I2C / SDMMC / SSD1677 register+RAM+BUSY
────────────────────────────────────────────────────────────────────
                              │
                              ▼
                    ESP32-S3 execution layer
          real 16MiB PlatformIO firmware flash image
                 Espressif QEMU / Xtensa
          ROM / bootloader / FreeRTOS / heap_caps / MMIO
                              │
                              ▼
                    physical Murphy M4
       EPD analog physics / RF / SD signal integrity / power
```

## 1. Source-of-truth policy

Hardware facts are not all equally strong. `board/murphy_m4.json` records their
provenance and preserves disagreement:

1. **production live-probe + firmware BoardConfig** — executable pin/controller
   truth;
2. **schematic** — physical topology and devices even when firmware disables a
   capability;
3. **field traces/issues** — timing/resource evidence;
4. **inference** — allowed only when explicitly labeled.

Example: the supplied schematic labels external flash as W25Q32 (nominal 4MiB),
while current production firmware and the N16R8 factory partition map use 16MiB.
The simulator uses 16MiB for executable firmware but deliberately keeps the
schematic discrepancy until a production JEDEC-ID trace resolves it.

Likewise, the schematic contains AHT20/SC7A20 devices, while current
`BoardConfig::MURPHY_M4` exposes `NO_SENSORS`. The board model can expose a
physical-view device without falsely advertising a firmware capability.

## 2. Dependency rule

Dependencies point inward:

1. `core/` — standard-C++ decisions, no backend dependency.
2. `platform/` — tiny I/O contracts.
3. `model/` — orchestration/lifetime state.
4. `hardware/`, `board/`, `native/` — concrete host backends.
5. `qemu/` — compiled ESP32-S3 execution/boot diagnostics.
6. `scenarios/`, `tests/` — compose the lowest truthful layer.

`ReaderModel` must never regain direct dependency on `SimPanel`/`SimStorage`.
The board controller layer must never absorb plugin/provider business rules.

## 3. Four different meanings of "display simulation"

These are intentionally separate:

### Reader/frame simulation — `SimPanel`

Answers:

- was a page-turn intent lost?
- which frame was rendered?
- did the physical-page record come from the committed frame or live target?
- did catch-up happen after BUSY?

It is fast, deterministic and schedule-fuzzable.

### SSD1677 digital controller — `SimSsd1677Controller`

Consumes command/data semantics matching the real FreeInk driver:

- BW/current RAM `0x24`;
- RED/old RAM `0x26`;
- RAM ranges/counters;
- LUT `0x32`;
- update controls `0x21/0x22`;
- MASTER_ACTIVATION `0x20`;
- active-high BUSY;
- one digital physical commit boundary per activation.

It catches invalid windows, incomplete RAM writes, activation while BUSY,
incorrect old/new plane provenance and controller-sequence errors.

### QEMU execution

Runs machine code and catches CPU/FreeRTOS/boot/heap/MMIO classes. It does not
become a Murphy board just because the CPU boots. `qemu/probe_boot.py` records
exactly how far the real image gets and what boundary fails.

### Physical EPD

Only a real panel can finally validate ghost density, analog gray progression,
temperature chemistry and whether a waveform visually looks correct.

## 4. Memory fidelity

`SimHeap` models independent internal banks plus PSRAM. It purposely distinguishes
`freeInternal()` from `largestInternal()` because field bugs occurred with plenty
of total/PSRAM free memory but no sufficiently large contiguous internal block.

`m4_issue_contracts` additionally encodes residency rules:

- framebuffer / large body / decode data → PSRAM;
- TLS/HTTP control plane and task stacks that require internal-capable memory →
  internal RAM;
- wrong placement is a `CONTRACT_VIOLATION`, not a best-effort warning.

Field observations live in `profiles/murphy_m4.json`; they are calibration
evidence, not hard-coded universal constants.

## 5. Persistence fidelity

SD is split into layers:

- `SimSdmmc` — card/rail/bus availability, asynchronous I/O, card removal,
  power-loss races;
- `SimStorage` — higher-level deterministic read latency/fault injection;
- `SimAtomicFileStore` — filesystem publication contract used by issue #24:
  rename fast path, bounded 2KiB fallback copy, sync/size verification and
  preservation of the previous authoritative generation.

This separation avoids pretending a bus controller implements FAT/exFAT
transaction semantics.

## 6. Reader/provider lifetime

`SimChapterLifecycle` tests the cross-chapter ownership rule behind issues #4/#5:

```text
open reader generation N
 -> store N+1 intent
 -> close/release generation N
 -> obtain + fully commit N+1 content
 -> open generation N+1
```

Empty or partial paths are states, not reader inputs. Long transition chains are
run repeatedly to catch generation/lifetime leaks.

## 7. Issue ownership

`issues/regressions.json` is machine validated. Status meanings:

- `reproduced`: a local executable scenario/CTest exercises the mechanism;
- `modeled`: relevant contract exists but exact field execution belongs to
  QEMU/device/upstream app tests;
- `device_trace_required`: missing evidence or physical/analog behavior;
- `fixed_upstream_regression`: the firmware/app repo owns the focused test.

`issues/plugin_regressions.json` maps Fanqie/JJWXC/WeRead/m4-device regressions
without copying Lua/provider logic into this repository.

## 8. Production migration state

The old architecture document described real-firmware shadow wiring as the next
step. That step now exists in `m4-firmware` PR #27:

- the real `TxtReaderActivity` remains authoritative;
- the same PageTurnCoordinator receives tap/render/commit/catch-up events;
- mismatches are logged as `[PTSH] DIFF`;
- full `pio run -e murphy_m4` passed.

The next production step is Phase B only after real-device shadow evidence is
clean: make the coordinator authoritative and retire duplicate page-turn state.

## 9. Real binary path

`tools/murphy_flash_image.py` composes the current Murphy 16MiB factory layout.
`qemu/run_full_flash.py` feeds it to Espressif ESP32-S3 QEMU. The dedicated
`qemu-real-firmware` workflow builds the actual `m4-firmware` Murphy target,
boots the image and archives/classifies the first hardware boundary.

The rule is: **model the boundary, rerun the same image**. Do not modify firmware
to skip a device just to make a QEMU screenshot appear.

## 10. Why multiple layers are better than one "complete" emulator

A single emulator optimized for realism becomes slow and nondeterministic for
race fuzzing; a pure mock is fast but misses compiled/SoC bugs. The stack keeps
both properties:

- deterministic layer: thousands of seeds, fault injection, temporal assertions;
- board layer: schematic/controller correctness;
- QEMU: actual machine code and ESP execution;
- physical unit: final analog/electrical truth.

A change graduates upward only when the lower relevant gates pass. That makes
physical flashing the final acceptance step rather than the primary debugger.
