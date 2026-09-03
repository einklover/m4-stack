# M4 P0 MemoryManager Baseline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Import the pinned upstream FreeInk MemoryManager safely, make the vendored dependency contract explicit, and add a low-overhead M4 memory snapshot/breadcrumb layer that can measure runtime pressure before any Wi-Fi/navigation behavior rewrite.

**Architecture:** Vendor MemoryManager as a new isolated SDK library without syncing existing hardware/display directories. Keep current `M4Psram` and heavy-gate behavior. Add an M4-specific telemetry facade that reads MemoryManager/ESP-IDF metrics and emits compact stage-tagged breadcrumbs; this facade is observational only in P0.

**Tech Stack:** C++20/Arduino on ESP32-S3, ESP-IDF heap capabilities, FreeRTOS, FreeInk MemoryManager, Python contract tests, PlatformIO.

**Spec:** `docs/superpowers/specs/2026-09-01-m4-system-runtime-optimization-design.md`

## Global Constraints

- Base implementation on `main` ref `b7dd293b761a96ea8be44486e82ed99173950bff` unless explicitly rebased by the coordinator.
- Upstream source is pinned to `Free-Ink/freeink-sdk@68425f8eec1246a0be0c0f311540f60ad733fa76`.
- Never replace `firmware/open-m4-sdk` wholesale.
- Do not modify same-name BoardConfig/InputManager/SDCardManager/FreeInkDisplay code in P0.
- Preserve current `M4Psram::createTask()` PSRAM-safe worker policy.
- Do not change Wi-Fi, HTTP, provider login, Activity navigation, or rendering behavior in P0.
- Do not flash hardware.
- `murphy_m4` is the production build contract; QEMU profiles are simulator-only.
- No private dependency downloads; all M4 build inputs remain vendored in-tree.

---

### Task 1: Make MemoryManager part of the vendored dependency contract

**Files:**
- Modify: `firmware/tests/test_m4_dependency_bootstrap_contract.py`
- Modify later in Task 2: `firmware/scripts/bootstrap_m4_deps.py`
- Modify later in Task 2: `scripts/bootstrap_deps.sh`

**Interfaces:**
- Consumes: current `REQUIRED_SENTINELS` contract in the Python test and both bootstrap scripts.
- Produces: required sentinel `open-m4-sdk/libs/hardware/MemoryManager/library.json`.

- [ ] **Step 1: Write the failing contract test**

Add this entry immediately after `InputManager` in the test's `REQUIRED_SENTINELS` tuple:

```python
"open-m4-sdk/libs/hardware/MemoryManager/library.json",
```

Add an assertion to `test_m4_pre_script_bootstraps_only_when_sentinels_are_missing()`:

```python
assert "open-m4-sdk/libs/hardware/MemoryManager/library.json" in module.REQUIRED_SENTINELS
```

- [ ] **Step 2: Run the test and verify RED**

Run from repo root:

```bash
python3 firmware/tests/test_m4_dependency_bootstrap_contract.py
```

Expected: FAIL because production bootstrap sentinel lists do not yet contain MemoryManager.

- [ ] **Step 3: Commit the RED test only**

```bash
git add firmware/tests/test_m4_dependency_bootstrap_contract.py
git commit -m "test(m4): require vendored MemoryManager"
```

---

### Task 2: Vendor the pinned FreeInk MemoryManager and make the contract GREEN

**Files:**
- Create: `firmware/open-m4-sdk/libs/hardware/MemoryManager/include/MemoryManager.h`
- Create: `firmware/open-m4-sdk/libs/hardware/MemoryManager/src/MemoryManager.cpp`
- Create: `firmware/open-m4-sdk/libs/hardware/MemoryManager/library.json`
- Modify: `firmware/scripts/bootstrap_m4_deps.py`
- Modify: `scripts/bootstrap_deps.sh`
- Modify: `firmware/open-m4-sdk/VENDOR.md`

**Interfaces:**
- Consumes: upstream `freeink::MemoryManager`, `freeink::MemPool`, `freeink::MemPressure`, `freeink::CacheSink`, `freeink::TaskStack` exactly as pinned.
- Produces: local include `<MemoryManager.h>` via `lib_extra_dirs = open-m4-sdk/libs/hardware`.

- [ ] **Step 1: Copy pinned upstream files byte-for-byte**

Copy only the three MemoryManager library files from the pinned upstream ref. Do not copy neighboring upstream hardware modules.

- [ ] **Step 2: Add the sentinel to both production validators**

Add exactly:

```python
"open-m4-sdk/libs/hardware/MemoryManager/library.json",
```

to `firmware/scripts/bootstrap_m4_deps.py`, and exactly:

```bash
"open-m4-sdk/libs/hardware/MemoryManager/library.json"
```

to `scripts/bootstrap_deps.sh`.

- [ ] **Step 3: Record provenance**

Append a VENDOR.md section containing the exact upstream repository, commit and imported path:

```markdown
### MemoryManager refresh

- Upstream: `Free-Ink/freeink-sdk`
- Commit: `68425f8eec1246a0be0c0f311540f60ad733fa76`
- Imported path: `libs/hardware/MemoryManager`
- Integration policy: isolated import only; no same-name hardware/display directories were overwritten.
```

- [ ] **Step 4: Verify the contract is GREEN**

```bash
python3 firmware/tests/test_m4_dependency_bootstrap_contract.py
```

Expected: PASS.

- [ ] **Step 5: Verify the production build**

```bash
cd firmware
pio run -e murphy_m4 -j1
```

Expected: PASS with no new unresolved dependency and no flash-layout change caused by the import alone.

- [ ] **Step 6: Commit**

```bash
git add firmware/open-m4-sdk/libs/hardware/MemoryManager \
  firmware/scripts/bootstrap_m4_deps.py scripts/bootstrap_deps.sh \
  firmware/open-m4-sdk/VENDOR.md
git commit -m "build(m4): vendor FreeInk MemoryManager"
```

---

### Task 3: Add a pure snapshot model contract before ESP-specific capture code

**Files:**
- Create: `firmware/src/util/M4RuntimeMemorySnapshot.h`
- Create: `firmware/tests/native_app/test_m4_runtime_memory_snapshot.cpp`

**Interfaces:**
- Produces:

```cpp
enum class M4MemoryPressure : uint8_t { None, Soft, Hard };

struct M4RuntimeMemorySnapshot {
  size_t internalFree;
  size_t internalLargest;
  size_t internalMinEver;
  size_t psramFree;
  size_t psramLargest;
  size_t psramMinEver;
  M4MemoryPressure pressure;
};

uint8_t m4InternalFragmentationPct(const M4RuntimeMemorySnapshot& snapshot);
```

- [ ] **Step 1: Write the failing native test**

```cpp
#include <assert.h>
#include "../../src/util/M4RuntimeMemorySnapshot.h"

int main() {
  M4RuntimeMemorySnapshot healthy{100000, 80000, 70000, 7000000, 6000000, 5000000,
                                  M4MemoryPressure::None};
  assert(m4InternalFragmentationPct(healthy) == 20);

  M4RuntimeMemorySnapshot fragmented{100000, 25000, 60000, 7000000, 6000000, 5000000,
                                     M4MemoryPressure::Soft};
  assert(m4InternalFragmentationPct(fragmented) == 75);

  M4RuntimeMemorySnapshot empty{0, 0, 0, 0, 0, 0, M4MemoryPressure::Hard};
  assert(m4InternalFragmentationPct(empty) == 100);
  return 0;
}
```

- [ ] **Step 2: Compile and verify RED**

Use the repository's native C++ test pattern with the exact compiler/include command used by neighboring `firmware/tests/native_app` tests. Expected: FAIL because the header/API does not exist.

- [ ] **Step 3: Implement the minimal pure header**

```cpp
#pragma once
#include <stddef.h>
#include <stdint.h>

enum class M4MemoryPressure : uint8_t { None, Soft, Hard };

struct M4RuntimeMemorySnapshot {
  size_t internalFree = 0;
  size_t internalLargest = 0;
  size_t internalMinEver = 0;
  size_t psramFree = 0;
  size_t psramLargest = 0;
  size_t psramMinEver = 0;
  M4MemoryPressure pressure = M4MemoryPressure::None;
};

inline uint8_t m4InternalFragmentationPct(const M4RuntimeMemorySnapshot& snapshot) {
  if (snapshot.internalFree == 0) return 100;
  const size_t largest = snapshot.internalLargest > snapshot.internalFree
                             ? snapshot.internalFree
                             : snapshot.internalLargest;
  return static_cast<uint8_t>(100U - ((largest * 100U) / snapshot.internalFree));
}
```

- [ ] **Step 4: Run native test and verify GREEN**

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add firmware/src/util/M4RuntimeMemorySnapshot.h \
  firmware/tests/native_app/test_m4_runtime_memory_snapshot.cpp
git commit -m "test(m4): define runtime memory snapshot contract"
```

---

### Task 4: Capture MemoryManager/ESP heap metrics without changing behavior

**Files:**
- Create: `firmware/src/util/M4RuntimeMemory.h`
- Create: `firmware/src/util/M4RuntimeMemory.cpp`
- Modify: `firmware/src/main.cpp` only at stable boot/navigation diagnostic hook points.

**Interfaces:**
- Consumes: `freeink::MemoryManager::instance()` and `M4RuntimeMemorySnapshot`.
- Produces:

```cpp
M4RuntimeMemorySnapshot m4CaptureRuntimeMemory();
void m4LogRuntimeMemory(const char* stage);
```

- [ ] **Step 1: Add a compile/contract test that includes the new facade and expects both symbols**

The test must fail because `M4RuntimeMemory.h` does not exist before implementation.

- [ ] **Step 2: Implement capture**

Map upstream pressure without exposing FreeInk types to callers:

```cpp
M4RuntimeMemorySnapshot m4CaptureRuntimeMemory() {
  auto& mm = freeink::MemoryManager::instance();
  M4RuntimeMemorySnapshot out;
  out.internalFree = mm.freeBytes(freeink::MemPool::Internal);
  out.internalLargest = mm.largestFreeBlock(freeink::MemPool::Internal);
  out.internalMinEver = mm.minEverFree(freeink::MemPool::Internal);
  out.psramFree = mm.freeBytes(freeink::MemPool::Psram);
  out.psramLargest = mm.largestFreeBlock(freeink::MemPool::Psram);
  out.psramMinEver = mm.minEverFree(freeink::MemPool::Psram);
  switch (mm.pressure()) {
    case freeink::MemPressure::Soft: out.pressure = M4MemoryPressure::Soft; break;
    case freeink::MemPressure::Hard: out.pressure = M4MemoryPressure::Hard; break;
    default: out.pressure = M4MemoryPressure::None; break;
  }
  return out;
}
```

`m4LogRuntimeMemory(stage)` prints one compact line containing stage, all six raw heap values, pressure, and computed internal fragmentation percentage. It must not allocate a large dynamic formatting buffer.

- [ ] **Step 3: Arm watermarks once after stable application initialization**

Call:

```cpp
freeink::MemoryManager::instance().setWatermarks(60, 75);
```

once after core display/input/storage initialization is complete. Do not call it before the reserved baseline is established.

- [ ] **Step 4: Add only observational breadcrumbs**

Add calls at existing stable boot/Home transition points only, for example:

```cpp
m4LogRuntimeMemory("boot-ready");
m4LogRuntimeMemory("home-enter");
```

Do not add automatic cache eviction or allocation rejection in P0.

- [ ] **Step 5: Run focused tests and production build**

```bash
python3 firmware/tests/test_m4_dependency_bootstrap_contract.py
cd firmware && pio run -e murphy_m4 -j1
```

Also run the new native snapshot test using the repository's native test command.

- [ ] **Step 6: Commit**

```bash
git add firmware/src/util/M4RuntimeMemory* firmware/src/main.cpp firmware/tests
git commit -m "feat(m4): add runtime memory pressure breadcrumbs"
```

---

### Task 5: Review evidence and open the P0 PR

**Files:**
- Update: issue #51 with command/results and before/after observations.
- No additional production file should be changed solely for this task unless review finds a defect.

- [ ] **Step 1: Run full required verification for this bounded change**

Minimum:

```bash
python3 firmware/tests/test_m4_dependency_bootstrap_contract.py
cd firmware && pio run -e murphy_m4 -j1
```

Run the relevant native tests and the smallest applicable QEMU smoke if the changed main hook is exercised there.

- [ ] **Step 2: Inspect diff for forbidden scope**

Reject the change if it modifies same-name upstream BoardConfig/InputManager/SDCardManager/FreeInkDisplay, Wi-Fi behavior, HTTP behavior, Activity teardown, provider polling behavior, or real-device flash tooling.

- [ ] **Step 3: Record measurements**

Attach available boot/Home internal-free, largest-block, min-ever, PSRAM and fragmentation data. If GitHub CI cannot produce hardware-only values, explicitly state that real-device runtime numbers remain pending rather than inventing them.

- [ ] **Step 4: Open PR referencing #51 and #50**

PR title:

```text
build(m4): adopt FreeInk MemoryManager and runtime baseline
```

PR body must include `Closes` only if #51 acceptance is fully satisfied; otherwise use `Refs #51` and leave the issue open.
