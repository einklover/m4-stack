# Resilient M4 UI Scene Framework Implementation Plan

> **For agentic workers:** Follow this plan in order. Use TDD for every
> behavior change. The current worktree is dirty; preserve all unrelated and
> pre-existing changes. Do not reset, revert, clean, commit, push, flash, or
> modify GitHub as part of this plan.

**Goal:** Extract the current Home Scene parser/executor into a reusable,
page-agnostic M4 UI Scene Framework whose renderer and input path never waits
for backend work, then migrate Home and prove reuse with a Settings mock scene.

**Spec:** `docs/superpowers/specs/2026-08-29-resilient-ui-scene-framework.md`

**Scope:** Framework foundation, compiler/preview extraction, Home adapter
migration, Settings mock proof, host/simulator resilience tests, and final
PlatformIO/simulator verification. Real Settings and Plugin screens are not
migrated in this plan.

## Existing ownership to preserve

- `firmware/src/activities/home/HomeActivity.*` currently combines activity
  lifecycle, display task, input, recent-book/progress/cover I/O, and Home
  rendering. The migration splits those responsibilities without changing
  Home action meanings.
- `firmware/src/activities/settings/SettingsActivity.*` remains the real
  Settings implementation in this plan. Its current vectors, settings
  pointers, subactivities, and save behavior are not rewritten.
- `firmware/src/activities/apps/NativeAppActivity.*`,
  `firmware/src/apps/native/M4NativeUi.*`, and
  `firmware/src/apps/native/M4NativeUiController.h` remain future consumer
  surfaces. Existing `revision()`/`pollAsync()` behavior is not expanded here.
- `firmware/src/util/HomeSceneRuntime.h` and the dirty compiler/tests are
  migrated through compatibility aliases, not deleted in the first step.

## File map

### Create

- `firmware/src/util/M4UiSceneRuntime.h` — canonical bounded M4TH/SCENE
  parser, node constants, numeric metadata helpers, and ordered visitors.
- `firmware/src/util/M4UiStateSnapshot.h` — fixed-size page-agnostic snapshot,
  value/list/item/text/asset records, and loading-state definitions.
- `firmware/src/util/M4UiStateStore.h` — public double-buffer store interface.
- `firmware/src/util/M4UiStateStore.cpp` — ESP32 bounded critical-section and
  host atomic publication implementation.
- `firmware/src/util/M4UiSceneExecutor.h` — pure page-adapter, renderer-sink,
  hit-test, and numeric-action interfaces.
- `firmware/src/util/M4UiSceneExecutor.cpp` — ordered node traversal and
  snapshot-only rendering/hit testing.
- `firmware/src/util/M4UiSceneController.h` — frame/input/system-navigation
  boundary and bounded action queue interface.
- `firmware/src/util/M4UiSceneController.cpp` — nonblocking controller logic.
- `firmware/src/activities/home/HomeSceneBackend.h` — Home backend task and
  fixed snapshot/asset publication declarations.
- `firmware/src/activities/home/HomeSceneBackend.cpp` — asynchronous recent
  books, progress, registry, and cover production.
- `firmware/src/activities/home/HomePageAdapter.h` — Home numeric binding and
  action map.
- `firmware/src/activities/home/HomePageAdapter.cpp` — pure snapshot lookup and
  action translation.
- `firmware/src/activities/settings/SettingsMockPageAdapter.h` — fixed mock
  Settings binding/action map used only by tests and preview.
- `firmware/tests/native_app/test_m4_ui_state_store.cpp` — publication atomicity,
  revision, and stable-copy tests.
- `firmware/tests/native_app/test_m4_ui_scene_framework.cpp` — generic ordered
  execution, numeric binding/action, and no-backend-call tests.
- `firmware/tests/native_app/test_m4_ui_scene_failure_injection.cpp` — stalled,
  error, empty, and stale backend responsiveness tests.
- `firmware/tests/native_app/fixtures/settings_mock_scene.json` — small scene
  using the same generic node/binding/action schema as Home.
- `firmware/tests/native_app/fixtures/m4_ui_scene_backend_probe.h` — test-only
  backend probe whose calls and stall mode are observable.
- `firmware/tools/compile_m4_ui_scene.py` — generic compiler implementation.
- `firmware/tools/preview_m4_ui_scene.py` — generic 480x800 preview tool.
- `simulator/tests/test_m4_ui_scene_resilience.py` — simulator contract and
  failure-injection journey assertions.

### Modify

- `firmware/src/util/HomeSceneRuntime.h` — compatibility include/aliases only.
- `firmware/tools/compile_home_theme.py` — compatibility wrapper delegating to
  `compile_m4_ui_scene.py`; preserve existing CLI flags and output bytes.
- `firmware/tools/preview_home_theme.py` — compatibility wrapper delegating to
  `preview_m4_ui_scene.py`.
- `firmware/src/activities/home/HomeActivity.h` — own the generic scene
  controller/store/backend handles and retain public Activity callbacks.
- `firmware/src/activities/home/HomeActivity.cpp` — remove backend work from
  render/input; use Home adapter/controller and ordered executor.
- `firmware/tests/native_app/test_home_scene_runtime.cpp` — include canonical
  runtime and retain M4TH/SCENE compatibility assertions.
- `firmware/tests/native_app/test_home_scene_executor.cpp` — assert snapshot-
  backed generic execution rather than provider-shaped callbacks.
- `firmware/tests/test_home_scene_compiler.py` — run through generic compiler
  and prove legacy CLI compatibility.
- `firmware/tests/test_preview_home_theme.py` — cover generic preview wrapper.
- `firmware/src/generated/murphy_default_m4theme.h` — regenerate only if the
  generic compiler produces byte-identical or intentionally reviewed output.
- `firmware/src/generated/mofei_classic_m4theme.h` — regenerate only when the
  generic compiler preserves the existing asset and scene contract.
- `simulator/m4sim.py` — add only the minimal preview/failure-injection command
  wiring required by the simulator test; do not expand QEMU behavior.

### Must not modify in this plan

`firmware/src/activities/settings/SettingsActivity.*`, all real plugin/native
provider activity files, reader files, network/provider implementations,
partition definitions, flashing scripts, SDK/library trees, `.pio`, binaries,
credentials, device captures, and unrelated dirty files.

## Interface contract to implement

Use these exact public concepts from the spec:

```cpp
namespace M4Ui {
using BindingId = uint8_t;
using ActionId = uint8_t;

enum class UiLoadState : uint8_t { Loading, Ready, Empty, Stale, Error };

class UiStateStore {
 public:
  bool publish(const UiStateSnapshot& next);
  bool copyLatest(UiStateSnapshot& out) const;
  uint32_t latestRevision() const;
  bool hasPublishedSnapshot() const;
};
}

namespace M4UiScene {
class PageAdapter {
 public:
  virtual bool value(BindingId, const UiStateSnapshot&, const ItemContext&,
                     ResolvedValue&) const = 0;
  virtual bool actionRect(ActionId, const UiStateSnapshot&, const ItemContext&,
                          Rect&) const = 0;
};

class UiActionSink {
 public:
  virtual bool enqueue(const UiAction&) = 0;
};

class M4UiSceneExecutor {
 public:
  bool render(const uint8_t*, size_t, const UiStateSnapshot&,
              const PageAdapter&, GfxRenderer&) const;
  bool hitTest(const uint8_t*, size_t, const UiStateSnapshot&,
               const PageAdapter&, int16_t, int16_t, UiAction*) const;
};
}
```

Implement fixed arrays and fixed-size text/assets exactly as specified. Do not
substitute `std::vector`, `std::string`, callbacks, or backend pointers into
the snapshot/action ABI.

## Task 1 — Extract generic parser core, preserving Home compatibility

**Files:**

- Create `firmware/src/util/M4UiSceneRuntime.h`.
- Modify `firmware/src/util/HomeSceneRuntime.h`.
- Modify `firmware/tools/compile_m4_ui_scene.py` and
  `firmware/tools/compile_home_theme.py`.
- Modify `firmware/tests/native_app/test_home_scene_runtime.cpp`.
- Modify `firmware/tests/test_home_scene_compiler.py`.
- Create `firmware/tests/native_app/test_m4_ui_scene_framework.cpp`.

### TDD steps

1. Add failing host assertions that the canonical namespace validates the
   current `murphy_default_m4theme`, preserves the 18-node serialized order,
   honors four-byte padding/repeat boundaries, rejects truncation, and exposes
   numeric visibility/action IDs.
2. Add a failing Python assertion that both compiler entry points emit the same
   M4TH bytes for the existing Home fixture and that arbitrary scene IDs are
   accepted by the generic compiler.
3. Run the RED tests:

   ```bash
   python3 -m pytest -q firmware/tests/test_home_scene_compiler.py firmware/tests/test_preview_home_theme.py
   c++ -std=c++17 -Ifirmware/src -Ifirmware/lib/GfxRenderer/src \
     firmware/tests/native_app/test_m4_ui_scene_framework.cpp -o /tmp/test_m4_ui_scene_framework
   /tmp/test_m4_ui_scene_framework
   ```

4. Move parser constants, section lookup, scene header validation, ordered
   command iteration, repeat iteration, and numeric metadata helpers into
   `M4UiSceneRuntime.h` with no allocation.
5. Make `HomeSceneRuntime.h` include the new header and provide aliases used by
   the dirty Home tests until each test is converted.
6. Extract compiler implementation into `compile_m4_ui_scene.py`. Keep
   `compile_home_theme.py` as a CLI-compatible wrapper and preserve generated
   Home package bytes unless a test documents a deliberate change.
7. Run the GREEN commands above, then:

   ```bash
   python3 -m py_compile firmware/tools/compile_m4_ui_scene.py firmware/tools/compile_home_theme.py
   git diff --check
   ```

**Exit condition:** generic parser/compiler tests pass and Home compatibility
tests still prove node order and package validation.

## Task 2 — Add immutable snapshots and bounded publication

**Files:**

- Create `firmware/src/util/M4UiStateSnapshot.h`.
- Create `firmware/src/util/M4UiStateStore.h`.
- Create `firmware/src/util/M4UiStateStore.cpp`.
- Create `firmware/tests/native_app/test_m4_ui_state_store.cpp`.

### TDD steps

1. Write failing host tests for:
   - no snapshot before first publication;
   - one complete `Loading` snapshot copied exactly;
   - monotonically observed revision changes;
   - concurrent publisher/reader copies never expose mixed revisions;
   - oversized text/list/asset counts are rejected without allocation;
   - the reader can keep rendering the prior snapshot while the publisher is
     stalled.
2. Build and run RED:

   ```bash
   c++ -std=c++17 -pthread -Ifirmware/src \
     firmware/src/util/M4UiStateStore.cpp \
     firmware/tests/native_app/test_m4_ui_state_store.cpp \
     -o /tmp/test_m4_ui_state_store
   /tmp/test_m4_ui_state_store
   ```

3. Implement two fixed `UiStateSnapshot` buffers. Use a short ESP32
   `portMUX_TYPE` critical section for bounded copy/index publication and a
   host atomic index/guard implementation under compile-time host guards.
4. Ensure `publish()` validates counts/text spans before copying and increments
   no heap allocation. Ensure `copyLatest()` copies one complete buffer while
   protected and returns false when none exists.
5. Run the GREEN command and inspect the size of the fixed snapshot buffers.
   Record any measured RAM cost in the implementation review notes, not in
   `HANDOFF.md`.

**Exit condition:** publication is bounded and complete, and a stalled backend
cannot prevent a reader from copying/rendering the last published snapshot.

## Task 3 — Implement snapshot-only executor and separate input/navigation

**Files:**

- Create `firmware/src/util/M4UiSceneExecutor.h`.
- Create `firmware/src/util/M4UiSceneExecutor.cpp`.
- Create `firmware/src/util/M4UiSceneController.h`.
- Create `firmware/src/util/M4UiSceneController.cpp`.
- Modify `firmware/tests/native_app/test_home_scene_executor.cpp`.
- Modify `firmware/tests/native_app/test_m4_ui_scene_framework.cpp`.

### TDD steps

1. Add a failing spy-renderer/spy-backend test. Its adapter increments a
   counter if a forbidden backend method is reached. The test must prove that
   `render()` and `hitTest()` can run with the backend stalled and the counter
   remains zero.
2. Add a failing ordered-event assertion using a synthetic scene whose bitmap
   node appears between text and line nodes. Assert the emitted paint events
   match JSON/M4TH order exactly.
3. Add a failing input test proving page input enqueues `{actionId,itemIndex}`
   and does not invoke the action. Add system-input tests for back/home/focus
   that succeed while the action queue is full.
4. Run RED:

   ```bash
   c++ -std=c++17 -pthread -Ifirmware/src \
     firmware/src/util/M4UiStateStore.cpp \
     firmware/src/util/M4UiSceneExecutor.cpp \
     firmware/src/util/M4UiSceneController.cpp \
     firmware/tests/native_app/test_m4_ui_scene_framework.cpp \
     -o /tmp/test_m4_ui_scene_framework
   /tmp/test_m4_ui_scene_framework
   ```

5. Implement the executor with scene bytes + copied snapshot + pure adapter
   only. Resolve scalar/list/item values from the snapshot. Do not call
   `controller_->rowCount()`, `SdMan`, provider code, plugin code, network,
   Lua, or any blocking lock from render/hit test.
6. Implement the controller with a bounded action ring buffer. Make system
   navigation independent of the ring buffer. Make `renderIfRequested()` copy
   one snapshot before traversal and never copy again mid-frame.
7. Treat missing data as content fallback states and always draw system chrome
   and navigation controls.
8. Run GREEN plus:

   ```bash
   python3 -m pytest -q simulator/tests/test_touch_regressions_contract.py
   git diff --check
   ```

**Exit condition:** executor and controller are page-agnostic, numeric, pure
on the UI side, and preserve ordered painting.

## Task 4 — Migrate Home behind a backend and Home adapter

**Files:**

- Create `firmware/src/activities/home/HomeSceneBackend.h` and `.cpp`.
- Create `firmware/src/activities/home/HomePageAdapter.h` and `.cpp`.
- Modify `firmware/src/activities/home/HomeActivity.h`.
- Modify `firmware/src/activities/home/HomeActivity.cpp`.
- Modify `firmware/tests/native_app/test_home_scene_executor.cpp`.
- Modify `firmware/tests/native_app/test_home_critical_ui.cpp` only when an
  existing Home geometry assertion needs the new boundary.

### TDD steps

1. Add failing Home contract tests asserting:
   - `onEnter()` publishes/requests a loading snapshot without reading recent
     books, registry, progress, or covers on the UI task;
   - first render is possible from loading data;
   - current/recent/app binding IDs resolve only from the copied snapshot;
   - Home action IDs retain existing callback meanings;
   - cover/template rendering is an ordered bitmap node, not a special final
     overlay branch;
   - input processing never calls `M4xRegistry::load()` or SD APIs.
2. Run the RED Home tests using the repository’s existing native test pattern.
   If the worktree has no shared native test runner for these files, compile
   each focused test explicitly with `c++ -std=c++17 -Ifirmware/src` and its
   existing include paths; do not introduce a new test framework.
3. Move recent-book enumeration, `loadBookProgress()`, registry lookup,
   thumbnail generation, and cover asset preparation into `HomeSceneBackend`.
   Publish `Loading` first, then `Ready`, `Empty`, `Stale`, or `Error`.
4. Replace direct Home rendering with `M4UiSceneController` and
   `M4UiSceneExecutor`. Keep existing `HomeRef`, `TouchHitGeometry`, theme
   geometry, footer masks, and activity callback targets as adapter inputs.
5. Convert the existing Mofei template/bitmap path into the generic `bitmap`
   scene node. The node order in the generated pack determines whether it is
   before or after dynamic nodes. Keep `HomeMofeiTemplateOverlay.h` only as the
   bounded bitmap source/helper if the executor needs it; remove its special
   Home-only render branch.
6. Preserve low-memory dialog/system controls as UI-owned controls. Any
   backend failure must not disable back/home/focus/render.
7. Drain page actions outside rendering. The action consumer may invoke the
   existing Home activity callbacks, but it must not run from executor or hit
   testing code.
8. Run GREEN focused Home tests, then:

   ```bash
   python3 -m pytest -q firmware/tests/test_home_scene_compiler.py
   python3 -m pytest -q simulator/tests/test_touch_regressions_contract.py
   git diff --check
   ```

**Exit condition:** Home preserves existing visual/action/touch contracts while
no UI render/input path performs backend I/O or waits for backend work.

## Task 5 — Settings mock scene proof without real Settings migration

**Files:**

- Create `firmware/src/activities/settings/SettingsMockPageAdapter.h`.
- Create `firmware/tests/native_app/fixtures/settings_mock_scene.json`.
- Create or extend `firmware/tests/native_app/test_m4_ui_scene_framework.cpp`.
- Extend `firmware/tools/preview_m4_ui_scene.py` if fixture preview needs a
  dedicated input option.

### TDD steps

1. Add a failing test that compiles the fixture using the generic compiler and
   renders it with a synthetic snapshot containing Display/Controls/System
   rows, selected index, and an `onChange` numeric action.
2. Assert the same executor handles header, tabs/list-like repeated rows,
   selected state, loading/empty/error content, and footer/system controls.
3. Assert the mock adapter maps numeric IDs without including or instantiating
   `SettingsActivity`.
4. Run RED, implement the fixed mock adapter/fixture, then run GREEN:

   ```bash
   python3 firmware/tools/compile_m4_ui_scene.py \
     firmware/tests/native_app/fixtures/settings_mock_scene.json \
     --out /tmp/settings_mock_scene.m4theme
   python3 firmware/tools/preview_m4_ui_scene.py \
     --m4theme /tmp/settings_mock_scene.m4theme \
     --out /tmp/settings_mock_scene.png
   /tmp/test_m4_ui_scene_framework
   ```

5. Confirm `git diff --name-only` contains no `SettingsActivity.*` change.

**Exit condition:** a second page uses the same runtime/store/executor without
runtime code changes and the real Settings activity remains untouched.

## Task 6 — Host and simulator failure-injection journeys

**Files:**

- Create `firmware/tests/native_app/test_m4_ui_scene_failure_injection.cpp`.
- Create `firmware/tests/native_app/fixtures/m4_ui_scene_backend_probe.h`.
- Create `simulator/tests/test_m4_ui_scene_resilience.py`.
- Modify `simulator/m4sim.py` only for minimal test/preview wiring.

### TDD steps

1. Write RED host tests with a backend probe in four modes: never returns,
   returns error, returns empty, and returns stale-after-refresh-failure.
2. For each mode, run repeated UI frames and input events. Assert:
   - frame count advances while backend is stalled;
   - focus moves and back/home events are handled within the bounded test tick;
   - page actions are queued, not executed by render/input;
   - loading/empty/stale/error state is content-only;
   - the last complete snapshot remains renderable.
3. Run the RED test, implement no-backend-call guards and bounded queue behavior,
   then run GREEN:

   ```bash
   c++ -std=c++17 -pthread -Ifirmware/src \
     firmware/src/util/M4UiStateStore.cpp \
     firmware/src/util/M4UiSceneExecutor.cpp \
     firmware/src/util/M4UiSceneController.cpp \
     firmware/tests/native_app/test_m4_ui_scene_failure_injection.cpp \
     -o /tmp/test_m4_ui_scene_failure_injection
   /tmp/test_m4_ui_scene_failure_injection
   ```

4. Add a simulator contract that runs the same fixture/backend probe through
   the host simulator event loop, captures frame/input timestamps, and fails if
   a stalled backend prevents a system response.
5. Run:

   ```bash
   python3 -m pytest -q simulator/tests/test_m4_ui_scene_resilience.py
   ```

**Exit condition:** host and simulator evidence show that stalled/error
backend work cannot freeze renderer, input, focus, back/home, or navigation.

## Task 7 — Generic preview and simulator visual check

**Files:**

- Create/modify `firmware/tools/preview_m4_ui_scene.py`.
- Modify `firmware/tools/preview_home_theme.py` as a compatibility wrapper.
- Modify `simulator/m4sim.py` only if needed for an explicit preview command.
- Do not add preview assets to firmware production sources.

### Steps

1. Preview the existing Home package and the Settings mock package at exactly
   480x800.
2. Verify ordered bitmap/template nodes, loading/error content, selected focus,
   footer/system controls, and CJK text bounds.
3. Use the established simulator path and reuse its build cache. Run the
   smallest relevant journey first:

   ```bash
   python3 firmware/tools/preview_m4_ui_scene.py \
     --m4theme /tmp/settings_mock_scene.m4theme \
     --out /tmp/settings_mock_scene.png
   ./m4sim test smoke --plugin-debug --skip-build --ready-seconds 90
   python3 -m pytest -q simulator/tests/test_m4_ui_scene_resilience.py
   ```

4. Review actual generated frames separately from host approximations. No
   visual-equivalence claim is made from `SMOKE PASS` alone.

**Exit condition:** Home and Settings mock previews are stable at 480x800 and
the simulator resilience journey passes.

## Task 8 — Final verification and scope audit

Run all commands from the repository root unless a command changes directory:

```bash
python3 firmware/tests/test_m4_dependency_bootstrap_contract.py
python3 -m pytest -q firmware/tests/test_home_scene_compiler.py firmware/tests/test_preview_home_theme.py
python3 -m pytest -q simulator/tests/test_m4_ui_scene_resilience.py simulator/tests/test_touch_regressions_contract.py
cmake -S simulator -B simulator/build
cmake --build simulator/build -j
ctest --test-dir simulator/build --output-on-failure
./m4sim test smoke --plugin-debug --skip-build --ready-seconds 90
cd firmware && /Users/zhouxinlai/.platformio/penv/bin/pio run -e murphy_m4 -j1
```

The final production build is compile evidence only; it is not hardware
evidence. Do not flash a device in this task.

Before handoff:

```bash
git diff --check
git status --short
```

Inspect the status output and confirm the only new/modified files attributable
to this plan are the listed framework/compiler/Home/test/preview files and the
two requested documents. Do not clean or revert pre-existing dirty files.

## Rollback points

Each task is independently revertible by its implementation change set:

1. parser/compiler extraction leaves the compatibility wrapper;
2. state store can be disabled while the old Home runtime remains available;
3. executor/controller can be bypassed by Home while tests retain the core;
4. Home backend/adapter migration can fall back to the old Home activity;
5. Settings mock and resilience tests have no production screen dependency.

No rollback step may use `git reset`, `git checkout`, `git clean`, or broad file
deletion. Do not commit or push as part of executing this plan.

## Concise completion summary

The implementation is complete when one bounded M4 UI Scene Framework renders
Home and the Settings mock from immutable numeric-ID snapshots, Home backend
work is asynchronous, system UI remains responsive under stalled/error
backends, simulator failure injection passes, the 480x800 previews are
reviewed, and the final `murphy_m4` PlatformIO build succeeds.
