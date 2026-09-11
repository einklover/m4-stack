# CrossMux-Derived Runtime Stability Phase 1 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port four proven CrossMux mechanisms into the m4-stack frame with zero user-visible behavior change except where an invariant acceptance test pins the fix: (1) RenderLock + `requestUpdateAndWait` discipline without a second render owner; (2) first-paint input settle with the corrected INV-I1 debounce sequencing; (3) existing `ButtonNavigator` unified via logical `NavNext`/`NavPrevious` at `MappedInputManager` edge level, synthetic-inclusive, with AppList explicit grid axes remaining explicit; (4) suppressed-release with frame-skip semantics.

**Architecture:** Keep the top-level lifecycle (`currentActivity`, `deferredDeleteActivity`, `enterNewActivity`, `exitActivity` in `firmware/src/main.cpp:412-430`, `ActivityWithSubactivity::pumpSubActivityFrame` in `firmware/src/activities/ActivityWithSubactivity.h:58`) and all per-activity render tasks exactly as today. Add a thin discipline layer beside them: one process-wide render mutex + RAII guard owned alongside `currentActivity` in `main.cpp`, one boot-only blocking primitive, two appended button enumerators, per-frame physical-only suppression state, and one boot-only two-sample settle step. No global render-dispatch flag, no synthetic FIFO, no governance wiring.

**Tech Stack:** C++17, Arduino-ESP32/PioArduino, FreeRTOS (`SemaphoreHandle_t`, `xTaskCreate`, `vTaskDelay`, `delay(10)`), `HalGPIO` debounced sampler (`DEBOUNCE_DELAY = 5 ms`), Python source-contract tests, C++20 host-model tests, `m4sim`/patched-QEMU journeys, `m4adb` one-shot synthetic contract.

**Spec:** `docs/superpowers/specs/2026-09-11-crossmux-runtime-stability-phase1-design.md` at `cf19660681c23414c8d2d5ca47f3cfb524935154` (authoritative; this plan defers to it on any conflict).

## Global Constraints

- Base exactly on design tip `cf19660681c23414c8d2d5ca47f3cfb524935154` (spec base `github/ui/touch-wifi-keyboard@8317a89`).
- Scope exactly four items above. Each task is independently reviewable and revertible; never bundle concurrency, input semantics, and navigation in one diff.
- Corrected INV-I1 rule is normative: two settle samples commit **level** (`currentState` / `isPressed`); edge masks (`pressedEvents` / `releasedEvents`) are **only** required clear after the first subsequent normal loop-owned `gpio.update()`.
- Render ownership: exactly one submitting context per activity instance. Global guard before any local mutex, never reverse. Display tasks use snapshot-under-local then render-plus-submit-under-global; synchronous paths take global-then-local in order. No path takes local-then-global.
- `requestUpdateAndWait` has exactly one caller (boot first paint), bounded wait exactly 2 s, CrossMux-identical misuse rules (assert in debug, serial diagnostic + immediate return in release), and exactly three allowed new boot serial lines (`[MAIN] First paint wait ok`, `[MAIN] First paint wait timeout, proceed to settle`, `[MAIN] Input settle done`). No existing serial line may change format.
- Navigator: `NavNext`/`NavPrevious` appended after `PageForward` (ordinals 0–8 unchanged, 11 enumerators total fit `uint16_t`). Edge-level equivalence, synthetic-inclusive, portrait policy, no orientation swap. AppList Left/Right stay explicit (install/uninstall); vertical moves use explicit-axis navigator overloads, not logical buttons.
- Suppression: physical-only; synthetic edges neither set, clear, nor observe suppression. Consumed frame returns before activity + sub-activity dispatch (frame-skip, not dispatch-rest); coincident edges/gestures dropped, held levels visible next frame, render tasks unaffected.
- Exclusions (must not enter in any task): synthetic FIFO/`InteractionBuffer` of any kind; one-shot synthetic semantics / 40 ms gate / `beginFrame` clear changes; process-wide render-dispatch flag / central task / shared dispatcher; wholesale `ActivityManager` port (stack, pending actions, `goTo…`, MainTab, exclusive storage); font work; touch-classifier widening (wt-touch-p0 seam untouched); plugin changes; static memory estimates / governance refusal; SDK sync (`firmware/open-m4-sdk`, `firmware/lib` untouched); broad `ActivityManager` port; QEMU input semantic changes; removal of any legacy path in the same diff as its replacement.
- No behavior flips without its invariant RED contract passing GREEN first. QEMU/`m4adb` suites run unchanged as regression gates. Allocation behavior and heap baselines unchanged.
- Do not flash hardware. Do not push. Commit the plan doc only on this planning branch; implementation commits go on the implementation branch in small frequent units (one RED commit + one minimal GREEN commit per item minimum).

## Context And Current Facts (mapped 2026-09-11, read-only)

- `firmware/src/activities/apps/AppListActivity.cpp:157-173` `displayTaskLoop` reads `updateRequired_`/`subActivity` under `renderingMutex_` every 10 ms; `:271-284` `selectIndex`/`moveSelection` write `selectedIndex_`/`updateRequired_` with no lock; `:233-262` `reload`/`onEnter`/`onExit` create/take/delete `renderingMutex_`; `:326-341` `openSelected`/`openInstall` take the mutex; `:434-478` touch/swipe/key dispatch calls `selectIndex`/`moveSelection` directly. Header `AppListActivity.h:78,81,88-89` owns `renderingMutex_`, `displayTaskLoop`, `selectIndex`, `moveSelection`. Grid constants `:34-35` (`kAppLongPressMs = 700`, `kDrawerColumns = 3`).
- `firmware/src/main.cpp:122` owns `currentActivity`; `:412-430` lifecycle trio; `:478-482` `waitForPowerRelease` (power-only loop); `:709+` `setup()`; `:1250-1269` home-or-reader branch pinned by `[MAIN] home1` / `[MAIN] reader`; setup tail calls `waitForPowerRelease()`; `:1281+` `loop()` runs `gpio.update()` → NTP state machine → power/deep-sleep checks → `:1517-1529` long-press Back-to-home (level + `getHeldTime() >= 1500`, `longPressBackHomeFired` latch) → `mappedInputManager.beginFrame()` → bridge poll → gesture routing → `currentActivity->loop()` → deferred-delete drain → `delay(10)`/`yield()`.
- `firmware/src/MappedInputManager.h:10` 9-value `Button` enum; `:29-32` `beginFrame`/`wasPressed`/`wasReleased`/`isPressed`; `:70-81` synthetic slot (`pulseSyntheticBack`, `injectSyntheticTap/Swipe/Key`); `:118` `kSynthMinIntervalMs = 40`. `firmware/src/MappedInputManager.cpp:36-80` `beginFrame` clears synth + tap/swipe caches; `:133-148` `injectSyntheticKey` one-shot + rate gate; `:149-175` `mapButton` hardware mapping; `:177-238` `wasPressed`/`wasReleased` (synthetic-Key + footer-tap Back/Confirm/Left/Right paths) / `isPressed`; `:246` `getHeldTime`.
- `firmware/src/util/ButtonNavigator.h:47-52` `getNextButtons() → {Down, Right}`, `getPreviousButtons() → {Up, Left}`; helpers `onNext/Previous(Press|Release|Continuous)`, generic `onPress/onRelease/onContinuous(Buttons, cb)`, index/page math. `ButtonNavigator.cpp` implements press as any-`wasPressed`, release as any-`wasReleased` gated by `lastContinuousNavTime`, continuous as any-`isPressed` + timing.
- Debounce source `firmware/open-m4-sdk/libs/hardware/InputManager/include/InputManager.h:242,244-245` (`currentState`, `pressedEvents`, `releasedEvents`); `:298` `DEBOUNCE_DELAY = 5`; `:24,27,30,36,50` `update()` / `isPressed` / `wasPressed` / `wasReleased` / `getHeldTime` (read-only; SDK not touched).
- Per-site adoption regions: AppList grid release edges `AppListActivity.cpp:460-478` (Up/Down ±`kDrawerColumns`, Left uninstall-dialog, Right install, Confirm open, Back back); `MyLibraryActivity` (`firmware/src/activities/home/MyLibraryActivity.cpp:680-756` preview/menu/action-menu release-edge rows, Up-or-Left previous / Down-or-Right next); Home press edges (`firmware/src/activities/home/HomeActivity.cpp:810-834` Up-or-Left previous / Down-or-Right next pressed, Confirm released, Back pressed early-return); Settings mixed (`firmware/src/activities/settings/SettingsActivity.cpp:194` Back pressed, `:291,303-306` Confirm released + Up/Down/Left/Right released with distinct per-direction handling).
- Sibling render owners: `MyLibraryActivity.h:15-16,19,33` (`displayTaskHandle`, `renderingMutex`, `updateRequired`, `displayTaskLoop`); `HomeActivity.h:24,32,42,44,78` (task handle, two `updateRequired` atomics, `renderingMutex`, `displayTaskLoop`). `ActivityWithSubactivity.h:58` `pumpSubActivityFrame`.
- Baseline tests (run unchanged as gates; not run during planning): `firmware/tests/test_m4_touch_wifi_ui_contract.py`, `firmware/tests/native_app/test_m4_touch_wifi_keyboard_contract.cpp` (CI builds with `g++ -std=c++20 … -o "$RUNNER_TEMP/…"`, `.github/workflows/m4-fast.yml:76-87`), `firmware/tests/test_m4_dependency_bootstrap_contract.py`, `firmware/tests/native_app/` (73 host-model `.cpp`), `firmware/tests/*.py` source contracts, `tests/contracts/memory_governance_contracts_red.cpp`, `simulator/tests/test_qemu_boot_probe.py`, `simulator/tests/synth_input_burst_e2e.py`, `./m4sim test smoke|network-manager|reader-ui --plugin-debug --skip-build --ready-seconds 90`.

### Task 1: INV-R1 RED — guarded-emission contracts (no behavior change)

**Files:**
- Create: `firmware/tests/native_app/test_phase1_r1_guard.cpp`
- Create: `firmware/tests/test_phase1_r1_guard_contract.py`

**Interfaces:**
- Consumes: current `AppListActivity.h:78,81,88-89`, `AppListActivity.cpp:160-173,271-284`, `main.cpp:122,412-430`.
- Produces: failing contracts pinning (a) all framebuffer mutation + display submission under a guard held by the single submitter; (b) `selectIndex`/`moveSelection` writes covered by the local mutex the display task reads under; (c) two-phase snapshot-then-submit (local snapshot, release local, global render+submit; local never held across submit); (d) lock order global-before-local.

- [ ] **Step 1: Write the failing host-model test**

  Model one activity instance with a recording renderer + instrumented local/global guards, K writer threads mutating selection under the local mutex while the submitter runs snapshot-then-submit. Assert painted highlight always equals protected `selectedIndex`, render generation increments exactly once per successful submit, no torn/interleaved submission.

- [ ] **Step 2: Write the failing source contract**

  Assert `firmware/src/util/M4RenderGuard.h` exists with move-forbidden RAII + explicit `unlock` + static `peek`; `main.cpp` owns one process-wide mutex alongside `currentActivity`; `AppListActivity.cpp` `selectIndex`/`moveSelection` bodies take the local mutex; `displayTaskLoop` snapshots under local, releases, then renders+submits under the global guard.

- [ ] **Step 3: RED commit and verify failure**

  Commands:

  ```bash
  g++ -std=c++20 firmware/tests/native_app/test_phase1_r1_guard.cpp -o /tmp/phase1_r1_guard && /tmp/phase1_r1_guard
  python3 firmware/tests/test_phase1_r1_guard_contract.py
  ```

  Expected: both fail (missing `M4RenderGuard.h`, unlocked `selectIndex` writes, no two-phase pattern). Non-zero exit is the RED gate.

- [ ] **Step 4: Commit RED only**

  `git add firmware/tests/native_app/test_phase1_r1_guard.cpp firmware/tests/test_phase1_r1_guard_contract.py && git commit -m "test(phase1-r1): RED guarded-emission contracts"`

### Task 2: Item 1 GREEN — M4RenderGuard + boot-only requestUpdateAndWait + AppList adoption (INV-R1)

**Files:**
- Create: `firmware/src/util/M4RenderGuard.h` (new; only new header this task)
- Modify: `firmware/src/main.cpp` (add process-wide mutex + primitive; exactly one boot call site in Task 4)
- Modify: `firmware/src/activities/apps/AppListActivity.cpp` (guard `selectIndex`/`moveSelection`, two-phase `displayTaskLoop`)
- Modify: `firmware/src/activities/apps/AppListActivity.h` (only if a guard-handle member is required; keep `renderingMutex_` name)

**Interfaces:**
- Produces (new, minimal):

  ```cpp
  // firmware/src/util/M4RenderGuard.h — mirrors CrossMux RenderLock, renamed owner.
  class M4RenderGuard final {
   public:
    explicit M4RenderGuard(SemaphoreHandle_t mutex, TickType_t wait = pdMS_TO_TICKS(100));
    ~M4RenderGuard();
    M4RenderGuard(const M4RenderGuard&) = delete;
    M4RenderGuard& operator=(const M4RenderGuard&) = delete;
    M4RenderGuard(M4RenderGuard&&) = delete;
    M4RenderGuard& operator=(M4RenderGuard&&) = delete;
    void unlock();
    [[nodiscard]] static bool peek(SemaphoreHandle_t mutex);
    [[nodiscard]] bool owns() const;
   private:
    SemaphoreHandle_t mutex_ = nullptr;
    bool owns_ = false;
  };

  // main.cpp — boot-only blocking primitive, CrossMux-identical misuse rules.
  bool requestUpdateAndWait(TickType_t timeout = pdMS_TO_TICKS(2000));
  ```

  Misuse: never from a render task or while holding the guard (assert in debug, serial diagnostic + immediate `false` return in release). Bounded waits only; no new `portMAX_DELAY`. Timeout path abandons the submit, never retries alongside a new owner.

- [ ] **Step 1: Add `M4RenderGuard` verbatim-port (name + owner only)**

  No policy logic inside the guard. Keep `unlock` + `peek` semantics identical to CrossMux `RenderLock`.

- [ ] **Step 2: Add the process-wide mutex + primitive skeleton in `main.cpp`**

  Mutex created alongside `currentActivity` (`main.cpp:122` region). Primitive runs the foreground render synchronously under guard on the main task and blocks until display submission returns (synchronous driver call, so the wait reduces to the guarded render). No per-frame caller, no global dispatch flag, no central task.

- [ ] **Step 3: Minimal AppList adoption (highest traffic + confirmed race only)**

  Take only the local mutex in `selectIndex` (`:271`) / `moveSelection` (`:278`); keep math identical. Rework `displayTaskLoop` (`:160-173`) to snapshot shared state under local, release local, then render+submit under the global guard. Synchronous paths take global-then-local in order. Nothing adds, removes, or reorders a submit call.

- [ ] **Step 4: GREEN verification + commit**

  Commands:

  ```bash
  g++ -std=c++20 firmware/tests/native_app/test_phase1_r1_guard.cpp -o /tmp/phase1_r1_guard && /tmp/phase1_r1_guard
  python3 firmware/tests/test_phase1_r1_guard_contract.py
  python3 firmware/tests/test_m4_touch_wifi_ui_contract.py
  ```

  Expected: all pass (exit 0). Then `git commit -m "feat(phase1-r1): guard AppList selection + two-phase submit"` — reverting this commit restores unlocked flags and deletes the shared mutex with no other item touched.

### Task 3: INV-I1 RED — settle + boot-order contracts (no behavior change)

**Files:**
- Create: `firmware/tests/native_app/test_phase1_i1_settle.cpp`
- Create: `firmware/tests/test_phase1_i1_boot_order_contract.py`

**Interfaces:**
- Consumes: `main.cpp:1250-1269` (home-or-reader branch), setup tail `waitForPowerRelease()` (`:478-482`), `loop():1281+` first `gpio.update()`, `InputManager.h:242,244-245,298`, `MappedInputManager.cpp:133-148` (synthetic one-shot).
- Produces: failing contracts pinning the corrected rule — (a) level held across two settle samples spaced past debounce commits to `currentState`/`isPressed`; (b) `pressedEvents`/`releasedEvents` NOT required clear after those two samples; both masks required clear only after the first subsequent normal loop-owned `gpio.update()` clears the latched edge unread; (c) setup order paint → settle → power-release wait; (d) synthetic events remain one-shot through settle.

- [ ] **Step 1: Write the failing sampler test**

  Drive a held level across two `update()` samples spaced 10 ms (past 5 ms debounce). Assert level committed; assert edges still latched after sample 2; assert both masks clear only after one more loop-owned `update()` with no read. Include a synthetic-Key control asserting one-shot semantics unaffected.

- [ ] **Step 2: Write the failing boot-order source contract**

  Assert `setup()` performs guarded first paint, then two-sample settle, then `waitForPowerRelease()`; assert settle sits after the `[MAIN] home1` / `[MAIN] reader` branch and before the power-release wait; assert the three serial markers exist and no other serial format changed.

- [ ] **Step 3: RED commit and verify failure**

  Commands:

  ```bash
  g++ -std=c++20 firmware/tests/native_app/test_phase1_i1_settle.cpp -o /tmp/phase1_i1_settle && /tmp/phase1_i1_settle
  python3 firmware/tests/test_phase1_i1_boot_order_contract.py
  ```

  Expected: both fail (no settle step, no markers, edges-cleared-too-early if naively asserted). Non-zero exit is the RED gate.

- [ ] **Step 4: Commit RED only**

  `git add firmware/tests/native_app/test_phase1_i1_settle.cpp firmware/tests/test_phase1_i1_boot_order_contract.py && git commit -m "test(phase1-i1): RED settle + boot-order contracts"`

### Task 4: Item 2 GREEN — boot blocking paint + two-sample settle (INV-I1)

**Files:**
- Modify: `firmware/src/main.cpp` (only file with behavior change this task: single boot call site + settle step + three serial lines)

**Interfaces:**
- Consumes: `requestUpdateAndWait` from Task 2, `M4RenderGuard`, `currentActivity` boot destination.
- Produces: boot sequence `enter initial activity → blocking guarded first paint (via requestUpdateAndWait, 2 s bound) → two-sample settle → waitForPowerRelease() → loop`. Diagnostics: success `[MAIN] First paint wait ok`, timeout `[MAIN] First paint wait timeout, proceed to settle`, settle done `[MAIN] Input settle done`, all in `[<millis>] [MAIN]` format. Timeout records failure in its own line and proceeds to settle without a second render owner (abandon, never retry alongside a new submitter). Duplicate submission prevented by ordering: guarded first paint runs before the destination display task starts, or the task submit path is gated on first-paint-complete — one holds, never neither.

- [ ] **Step 1: Add the single boot call site**

  After the home-or-reader branch (`:1250-1269`), before `waitForPowerRelease()`. Enter initial activity already done by `onGoHome()` / `onGoToReader()`; run its render synchronously under guard on the main task via `requestUpdateAndWait()`.

- [ ] **Step 2: Add the two-sample settle (corrected sequencing)**

  Two `gpio.update()` samples spaced `delay(10)` (existing 10 ms idiom, 20 ms wall total, boot-only, never on hot path). After sample 2, level is committed; do NOT assert or force edge masks clear here — the first subsequent normal loop-owned `gpio.update()` in `loop()` clears the latched edge unread, so first dispatched frames see `isPressed` with silent edges. Synthetic path untouched by construction (bypasses debounced sampler). Covers whichever activity actually starts (including Back-held-to-home) because it runs after the branch decision.

- [ ] **Step 3: GREEN verification + commit**

  Commands:

  ```bash
  g++ -std=c++20 firmware/tests/native_app/test_phase1_i1_settle.cpp -o /tmp/phase1_i1_settle && /tmp/phase1_i1_settle
  python3 firmware/tests/test_phase1_i1_boot_order_contract.py
  python3 firmware/tests/test_phase1_r1_guard_contract.py
  ```

  Expected: all pass. Then `git commit -m "feat(phase1-i1): boot blocking paint + two-sample settle"` — reverting deletes the two-sample step and the single boot call site; boot returns to enter-then-wait-for-release with no other item touched. QEMU boot-log check is deferred to Task 10 (no timing changes pinned).

### Task 5: INV-N1 RED — logical-navigation equivalence contracts (no behavior change)

**Files:**
- Create: `firmware/tests/native_app/test_phase1_n1_nav_equivalence.cpp`
- Create: `firmware/tests/test_phase1_n1_adoption_contract.py`

**Interfaces:**
- Consumes: `MappedInputManager.h:10` enum, `:177-238` edge/level getters + synthetic-Key path, `ButtonNavigator.h:47-52` + `ButtonNavigator.cpp` press/release/continuous + index/page math, per-site regions (AppList `:460-478`, MyLibrary `680-756`, Home `:810-834`, Settings `:194,291,303-306`).
- Produces: failing contracts pinning (a) `NavNext` == Down-or-Right and `NavPrevious` == Up-or-Left on `wasPressed`, `wasReleased`, and `isPressed` (level-OR), over every button-by-edge combination; (b) synthetic Keys for Down/Right/Up/Left read through `NavNext`/`NavPrevious` exactly as through members, and synthetic injection never touches suppression state; (c) AppList grid binding — vertical moves step one row (`±kDrawerColumns = ±3`), Left/Right keep uninstall/install bindings; (d) `getNextButtons() → {NavNext}`, `getPreviousButtons() → {NavPrevious}` singletons, index/page math untouched, portrait policy, no axis swap.

- [ ] **Step 1: Write the failing equivalence test**

  Enumerate all 11 buttons × {pressed, released, level} through legacy getters vs logical mapping; assert identical outcomes. Drive synthetic-Key Down/Right/Up/Left and assert they read as `NavNext`/`NavPrevious` press+release in the same frame (mirroring member-button one-shot pulse). Assert synthetic sequence leaves suppression state clear.

- [ ] **Step 2: Write the failing adoption source contract**

  Assert `Button` enum appends `NavNext, NavPrevious` after `PageForward` with ordinals 0–8 unchanged; `getNext/PreviousButtons` return the single logical each; AppList vertical sites use explicit-axis navigator overloads (`onRelease({Down})` / `onRelease({Up})` or equivalent explicit sets, never logical for Left/Right); MyLibrary uses `onNext/PreviousRelease` logical; Home uses `onNext/PreviousPress` logical; Settings converts only pure previous/next pairs.

- [ ] **Step 3: RED commit and verify failure**

  Commands:

  ```bash
  g++ -std=c++20 firmware/tests/native_app/test_phase1_n1_nav_equivalence.cpp -o /tmp/phase1_n1_nav && /tmp/phase1_n1_nav
  python3 firmware/tests/test_phase1_n1_adoption_contract.py
  ```

  Expected: both fail (missing enumerators, raw Down/Right lists, hand-rolled edge checks). Non-zero exit is the RED gate.

- [ ] **Step 4: Commit RED only**

  `git add firmware/tests/native_app/test_phase1_n1_nav_equivalence.cpp firmware/tests/test_phase1_n1_adoption_contract.py && git commit -m "test(phase1-n1): RED logical-navigation contracts"`

### Task 6: Item 3 GREEN — NavNext/NavPrevious + ButtonNavigator unification (INV-N1)

**Files:**
- Modify: `firmware/src/MappedInputManager.h` (enum append only) + `firmware/src/MappedInputManager.cpp` (`mapButton` cases for the two new enumerators as OR over member sets at edge level; `isPressed` level-OR; synthetic-Key path covers them automatically via `synthKey_ == button`)
- Modify: `firmware/src/util/ButtonNavigator.h` (`getNextButtons` → `{NavNext}`, `getPreviousButtons` → `{NavPrevious}` only) — helpers and index/page math untouched
- Modify per-site dispatch only (no other logic): `firmware/src/activities/apps/AppListActivity.cpp:460-478`, `firmware/src/activities/home/MyLibraryActivity.cpp:680-756` rows, `firmware/src/activities/home/HomeActivity.cpp:810-834`, `firmware/src/activities/settings/SettingsActivity.cpp:194,291,303-306` (+ same pattern for remaining settings rows: convert only pure pairs)

**Interfaces:**
- Consumes: RED contracts from Task 5.
- Produces:

  ```cpp
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward, NavNext, NavPrevious };
  // wasPressed(NavNext) == wasPressed(Down) || wasPressed(Right), synthetic-inclusive; ditto NavPrevious == Up || Left.
  // wasReleased symmetric; isPressed symmetric level-OR. No orientation swap in Phase 1.
  ```

  Adoption table (edge kinds preserved; only the dispatch helper changes):
  - AppList grid: vertical moves via explicit-axis overloads over Down/Up edge sets (`onRelease({Down})`/`onRelease({Up})` stepping `±kDrawerColumns`); Left (uninstall dialog, plugin-gated), Right (install), Confirm (open), Back (back) keep exact bindings through navigator helpers. Left/Right never fold into logical.
  - MyLibrary rows: `onPreviousRelease` / `onNextRelease` logical. Preview-menu rows keep menu-index behavior.
  - Home lists: `onPreviousPress` / `onNextPress` logical; Confirm/Back unchanged (press-edge kind preserved).
  - Settings: Up/Down pairs to logical release overloads; Left/Right keep explicit bindings where distinct; Back/Confirm unchanged. Inert logical enumerators left unused by any rollback may be removed in a later cleanup; while present they change no dispatch. No second navigator is created.

- [ ] **Step 1: Extend the enum + mapper (ordinals preserved)**

  Append only. Eleven enumerators fit `uint16_t` suppression/edge masks with room to spare. Verify ordinals 0–8 unchanged by static assert or contract.

- [ ] **Step 2: Flip the two getters to logical singletons**

  No other `ButtonNavigator` change. Confirm continuous navigation still flows through `isPressed` level-OR over the same member sets.

- [ ] **Step 3: Convert sites per table, preserving edge kinds**

  Press-edge sites use the press family; release-edge sites use the release family. No edge-kind migration. Keep swipe paths (`AppListActivity.cpp:449-452`) and touch paths unchanged.

- [ ] **Step 4: GREEN verification + commit**

  Commands:

  ```bash
  g++ -std=c++20 firmware/tests/native_app/test_phase1_n1_nav_equivalence.cpp -o /tmp/phase1_n1_nav && /tmp/phase1_n1_nav
  python3 firmware/tests/test_phase1_n1_adoption_contract.py
  python3 firmware/tests/test_phase1_r1_guard_contract.py
  ```

  Expected: all pass. Then `git commit -m "feat(phase1-n1): logical NavNext/NavPrevious unification"` — reverting the two getters to raw lists restores prior dispatch; appended enumerators stay inert.

### Task 7: INV-S1 RED — consume-once + frame-skip contracts (no behavior change)

**Files:**
- Create: `firmware/tests/native_app/test_phase1_s1_suppress.cpp`
- Create: `firmware/tests/test_phase1_s1_frame_skip_contract.py`

**Interfaces:**
- Consumes: `main.cpp:1517-1529` long-press Back site, `MappedInputManager.h:29-32,70-81` frame/synthetic paths, `main.cpp:1560-1660` frame tail (gesture → `currentActivity->loop()` → deferred-delete → `delay(10)`/`yield()`).
- Produces: failing contracts pinning (a) press → hold past threshold → release yields exactly one callback, a consumed release, and a subsequent fresh press firing again; (b) mixed-frame: unrelated button edge + gesture pulse coincident with the consumed release frame are both dropped while held level reads correctly next frame; (c) synthetic-Key sequences around a suppressed release neither set, clear, nor observe suppression; (d) frame-top check placement between gesture handling and `currentActivity->loop()` with frame-skip return before activity + sub-activity dispatch; render tasks independent of the skip.

- [ ] **Step 1: Write the failing host-model test**

  Drive press → hold past threshold → release through a frame-top `consumeSuppressedRelease()` model; assert one callback, consumed release, fresh-press-again works. Drive a mixed frame (unrelated edge + gesture pulse + suppressed release) and assert both dropped, next-frame level reads correct.

- [ ] **Step 2: Write the failing source contract**

  Assert `MappedInputManager` exposes physical-only suppression (`suppressNextRelease`, `consumeSuppressedRelease`, per-button long-press one-shot state over a `uint16_t` mask, no BLE edge arrays); `main.cpp` frame-top check sits between gesture handling and `currentActivity->loop()` and returns before dispatch on consume; the Back 1.5 s site keeps threshold + destination bit-for-bit with only firing/release-consumption changed; synthetic paths never reference suppression state.

- [ ] **Step 3: RED commit and verify failure**

  Commands:

  ```bash
  g++ -std=c++20 firmware/tests/native_app/test_phase1_s1_suppress.cpp -o /tmp/phase1_s1_suppress && /tmp/phase1_s1_suppress
  python3 firmware/tests/test_phase1_s1_frame_skip_contract.py
  ```

  Expected: both fail (no suppression state, no frame-top check, ad-hoc level+timestamp Back handling). Non-zero exit is the RED gate.

- [ ] **Step 4: Commit RED only**

  `git add firmware/tests/native_app/test_phase1_s1_suppress.cpp firmware/tests/test_phase1_s1_frame_skip_contract.py && git commit -m "test(phase1-s1): RED consume-once + frame-skip contracts"`

### Task 8: Item 4 GREEN — suppression state + frame-top check + one Back site (INV-S1)

**Files:**
- Modify: `firmware/src/MappedInputManager.h` + `firmware/src/MappedInputManager.cpp` (add physical-only suppression state; no synthetic-path change)
- Modify: `firmware/src/main.cpp` (add frame-top `consumeSuppressedRelease()` check + convert the single long-press Back-to-home site `:1517-1529` only)

**Interfaces:**
- Produces (minimal, CrossMux-adapted minus BLE arrays):

  ```cpp
  // MappedInputManager — physical-button, long-press state only.
  bool wasLongPressed(Button button, unsigned long thresholdMs) const; // one-shot threshold event while held
  void suppressNextRelease(Button button);      // record consumed press; physical only
  bool consumeSuppressedRelease();              // frame-top check; true => skip frame (before activity dispatch)
  // Synthetic edges never set/clear/observe suppression. 11 enumerators fit uint16_t mask.
  ```

  Frame semantics (CrossMux frame-skip): when the check reports consumed, `loop()` returns before activity and sub-activity dispatch; coincident unrelated edges/gesture pulses in that frame are dropped (not deferred/replayed); held levels observable next frame via normal `isPressed`; next fresh press works normally; render tasks run independently. Converted site keeps 1.5 s threshold and `onGoHome()` destination bit-for-bit; only the firing + release-consumption mechanism changes (replaces ad-hoc `isPressed` + `getHeldTime` + `longPressBackHomeFired` with the new state).

- [ ] **Step 1: Add suppression state (default off, single toggle)**

  Default the frame-top check off in one place so rollback is a one-line default flip; long-press sites keep current level-read behavior until converted.

- [ ] **Step 2: Place the frame-top check exactly between gesture handling and `currentActivity->loop()`**

  On consume, return before `currentActivity->loop()` (hence before `pumpSubActivityFrame` paths). Do not rest dispatch, do not defer edges.

- [ ] **Step 3: Convert only the verified Back 1.5 s site**

  No other long-press site is touched. Prove synthetic inertness (existing synthetic journey assertions unchanged).

- [ ] **Step 4: GREEN verification + commit**

  Commands:

  ```bash
  g++ -std=c++20 firmware/tests/native_app/test_phase1_s1_suppress.cpp -o /tmp/phase1_s1_suppress && /tmp/phase1_s1_suppress
  python3 firmware/tests/test_phase1_s1_frame_skip_contract.py
  python3 firmware/tests/test_phase1_n1_adoption_contract.py
  ```

  Expected: all pass. Then `git commit -m "feat(phase1-s1): consume-once release + frame-top skip"` — reverting defaults the check off; Back site keeps level-read behavior.

### Task 9: Top-three closeout — MyLibrary + Home guard + navigator remainder (INV-R1 + INV-N1)

**Files:**
- Modify: `firmware/src/activities/home/MyLibraryActivity.h:15-16,19,33` + `MyLibraryActivity.cpp` (guard adoption + logical release cutover for linear rows only)
- Modify: `firmware/src/activities/home/HomeActivity.h:24,32,42,44,78` + `HomeActivity.cpp:810-834` (guard adoption with backend-scene-task attention + logical press cutover)
- No other activity is touched; remaining activities adopt only as needed in later work, each behind its own contracts. Unmigrated activities behave exactly as today (all new mechanisms default off).

**Interfaces:**
- Consumes: `M4RenderGuard` + logical buttons from Tasks 2/6.
- Produces: same two-phase + lock-order pattern as AppList, applied per activity behind that activity's contracts; `ActivityWithSubactivity` display paths add guard acquisitions around existing submit calls (`displayTaskLoop` already skips when a sub-activity is present) rather than new submits. Child rendering follows the single-owner rule.

- [ ] **Step 1: MyLibrary guard + logical release (self-contained diff)**

  Linear rows take `onNextRelease` / `onPreviousRelease`; preview/action-menu rows keep menu-index behavior. Guard shared framebuffer state per the two-phase pattern.

- [ ] **Step 2: Home guard + logical press (self-contained diff)**

  Lists take `onNextPress` / `onPreviousPress`; Confirm/Back unchanged. Guard framebuffer state shared with backend scene tasks (`backendCtx->updateRequired` / `updateRequired` paths in `:810-834`); do not change wake mechanisms.

- [ ] **Step 3: Verify + two commits (one per activity)**

  Commands per activity:

  ```bash
  g++ -std=c++20 firmware/tests/native_app/test_phase1_r1_guard.cpp -o /tmp/phase1_r1_guard && /tmp/phase1_r1_guard
  g++ -std=c++20 firmware/tests/native_app/test_phase1_n1_nav_equivalence.cpp -o /tmp/phase1_n1_nav && /tmp/phase1_n1_nav
  python3 firmware/tests/test_phase1_r1_guard_contract.py
  python3 firmware/tests/test_phase1_n1_adoption_contract.py
  ```

  Expected: all pass. Commits: `git commit -m "feat(phase1): MyLibrary guard + navigator"` then `git commit -m "feat(phase1): Home guard + navigator"` — each reverts without touching the other or Tasks 2/4/6/8.

### Task 10: Final integration/regression — PR70 touch/Wi-Fi + QEMU/m4adb gates (no new behavior)

**Files:**
- Modify: none (evidence task; failures here block the phase, they do not authorize scope expansion).

**Interfaces:**
- Consumes: all prior tasks GREEN.
- Produces: green gates proving behavior-identical except invariant fixes: PR70 touch/Wi-Fi contracts, full QEMU journey suite with strict serial-anomaly rule (no new lines, no changed formats — only the three named boot markers may appear as additions), synthetic one-shot journeys unchanged, memory-governance contracts untouched, no journey timing adjustments.

- [ ] **Step 1: Host + source contracts (fast gates first)**

  ```bash
  python3 firmware/tests/test_m4_dependency_bootstrap_contract.py
  python3 firmware/tests/test_m4_touch_wifi_ui_contract.py
  g++ -std=c++20 firmware/tests/native_app/test_m4_touch_wifi_keyboard_contract.cpp -o /tmp/touch-kb && /tmp/touch-kb
  python3 firmware/tests/test_m4_file_transfer_service_ownership_contract.py
  python3 firmware/tests/test_m4_esp_http_server_contract.py
  python3 firmware/tests/test_m4_navigation_supervisor_contract.py
  python3 firmware/tests/test_m4_memory_governance_contract.py
  g++ -std=c++20 firmware/tests/native_app/test_phase1_r1_guard.cpp -o /tmp/phase1_r1_guard && /tmp/phase1_r1_guard
  g++ -std=c++20 firmware/tests/native_app/test_phase1_i1_settle.cpp -o /tmp/phase1_i1_settle && /tmp/phase1_i1_settle
  g++ -std=c++20 firmware/tests/native_app/test_phase1_n1_nav_equivalence.cpp -o /tmp/phase1_n1_nav && /tmp/phase1_n1_nav
  g++ -std=c++20 firmware/tests/native_app/test_phase1_s1_suppress.cpp -o /tmp/phase1_s1_suppress && /tmp/phase1_s1_suppress
  ```

  Expected: all pass (exit 0). Any failure stops the phase; fix inside the owning task, never by widening scope.

- [ ] **Step 2: QEMU/m4adb regression (unchanged suites)**

  ```bash
  cd firmware && pio run -e murphy_m4_qemu_plugin -j 2
  ./m4sim test smoke --plugin-debug --skip-build --ready-seconds 90
  ./m4sim test network-manager --plugin-debug --skip-build --ready-seconds 90
  ./m4sim test reader-ui --plugin-debug --skip-build --ready-seconds 90
  python3 simulator/tests/synth_input_burst_e2e.py
  python3 simulator/tests/test_qemu_boot_probe.py
  ```

  Expected: green throughout; QEMU boot log shows setup paint → settle → power-release order with only the three allowed new `[MAIN]` markers; MyLibrary down/confirm/back journey green without timing adjustments; long-press Back (home, 1.5 s) journey green with threshold unchanged; AppList grid vertical ±1 row and Left/Right install/uninstall bindings intact.

- [ ] **Step 3: Record evidence, no code change**

  Capture exact heads, CI run IDs, and boot-log excerpts in the Task 10 report. Commit nothing in this task unless updating the plan's evidence appendix is explicitly requested.

## Risks / Rollback

- Lock-order inversion (global-after-local somewhere) → deadlock. Mitigation: Tasks 2/9 enforce global-before-local by construction; RED contract pins order; bounded waits only on new paths.
- Second render owner on boot timeout → torn first frame. Mitigation: Task 4 abandons the timed-out submit, never retries alongside a new one; one of paint-before-task-start vs first-paint-complete gate always holds.
- Edge-kind migration (press↔release) silently changing UX. Mitigation: Tasks 5/6/9 preserve edge kinds per site; AppList Left/Right never fold into logical.
- Suppression swallowing unrelated input beyond one frame. Mitigation: Task 8 frame-skip drops only the coincident frame; levels re-observable next frame; synthetic inert; default-off toggle gives one-line rollback.
- Per-item rollback (spec §Rollback boundaries): guard → remove acquisitions, restore flags, delete mutex; primitive → delete single boot call site; settle → delete two-sample step; navigator → revert two getters (enumerators inert); suppression → default check off. No legacy path is removed in the same diff as its replacement.

## Open Questions

None. All material choices are pinned by the spec tip `cf19660` and the mapped baselines above. If implementation uncovers a spec/plan conflict, halt that task and surface it before proceeding — the spec is authoritative.

## Plan self-review (pre-commit)

- Spec coverage: Item 1 (Tasks 1–2) covers RenderLock port, lock order, two-phase pattern, misuse rules, 2 s bound, single owner incl. timeout; Item 2 (Tasks 3–4) covers corrected INV-I1 level-vs-edge rule, placement after home/reader decision, 10 ms spacing, 20 ms cost, synthetic unaffected, three serial markers, no-format-change; Item 3 (Tasks 5–6 + 9 remainder) covers 11-enumerator append, edge-level synthetic-inclusive equivalence, portrait policy, singleton getters, per-site table with AppList explicit axes, INV-N1 acceptance (1)–(4); Item 4 (Tasks 7–8) covers physical-only suppression, `uint16_t` mask, Back 1.5 s site bit-for-bit threshold/destination, frame-top placement, frame-skip drop semantics, held-levels-next-frame, synthetic inertness, INV-S1 acceptance (1)–(4). Migration order (AppList → MyLibrary → Home) and TDD rhythm (RED before GREEN, QEMU/m4adb unchanged gates) preserved.
- Placeholders: none — every task names exact files, line regions, interfaces, commands, expected exits, and commit messages.
- Interface consistency: `M4RenderGuard` (move-forbidden, `unlock`, `peek`) used identically in Tasks 1–2/4/9; `requestUpdateAndWait(2000 ms)` single boot caller; `Button` append-only with `uint16_t` capacity shared by Tasks 5–8; suppression trio (`wasLongPressed`/`suppressNextRelease`/`consumeSuppressedRelease`) physical-only shared by Tasks 7–8/10; no global dispatch flag, FIFO, governance, font, touch, plugin, or SDK surface introduced.
- Plan/spec conflicts found: none. The corrected INV-I1 latch contract in the tip (`level commits after two samples; edges clear only after the first subsequent normal loop-owned gpio.update()`) is reproduced verbatim in Tasks 3–4 and contradicts any earlier "edges clear after settle" reading — the tip reading governs.




