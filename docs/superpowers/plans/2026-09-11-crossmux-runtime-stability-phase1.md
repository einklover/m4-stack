# CrossMux-Derived Runtime Stability Phase 1 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port four proven CrossMux mechanisms into the m4-stack frame with zero user-visible behavior change except where an invariant acceptance test pins the fix: (1) RenderLock + boot-only blocking-primitive discipline without a second render owner; (2) first-paint input settle with the corrected INV-I1 debounce sequencing; (3) existing `ButtonNavigator` unified via logical `NavNext`/`NavPrevious` at `MappedInputManager` edge level, synthetic-inclusive, with AppList explicit grid axes remaining explicit; (4) suppressed-release with frame-skip-to-tail semantics.

**Architecture:** Keep the top-level lifecycle (`currentActivity`, `deferredDeleteActivity`, `enterNewActivity`, `exitActivity` in `firmware/src/main.cpp:412-430`, `ActivityWithSubactivity::pumpSubActivityFrame` in `firmware/src/activities/ActivityWithSubactivity.h:58`), the `Activity` public API (`onEnter`/`onExit`/`loop` only — there is deliberately no public render entry), and all per-activity render tasks exactly as today. Add a thin discipline layer beside them: one process-wide render mutex + RAII guard owned alongside `currentActivity` in `main.cpp`, one boot-only completion wait on the existing owning display task (no synchronous render, no new render methods), two appended button enumerators resolved by a dependency-free mapping helper, per-frame physical-only suppression state driven by a dependency-free state helper, and one boot-only two-sample settle step. No global render-dispatch flag, no synthetic FIFO, no governance wiring, no central ActivityManager.

**Tech Stack:** C++17, Arduino-ESP32/PioArduino, FreeRTOS (`SemaphoreHandle_t`, `xTaskCreate`, `vTaskDelay`, `delay(10)`), `HalGPIO` debounced sampler (`DEBOUNCE_DELAY = 5 ms`), dependency-free pure helpers (`util/M4NavMapping.h`, `util/M4SuppressState.h`, zero Arduino/FreeRTOS includes, tested with bare `g++ -std=c++20 -Ifirmware/src` per the `test_progressive_http_state` / `test_m4_touch_wifi_keyboard_contract` precedent), Python source-contract tests, `m4sim`/patched-QEMU journeys, `m4adb` one-shot synthetic contract.

**Spec:** `docs/superpowers/specs/2026-09-11-crossmux-runtime-stability-phase1-design.md` at `cf19660681c23414c8d2d5ca47f3cfb524935154` (authoritative; this plan defers to it on any conflict).

## Global Constraints

- Base exactly on design tip `cf19660681c23414c8d2d5ca47f3cfb524935154` (spec base `github/ui/touch-wifi-keyboard@8317a89`).
- Scope exactly four items above. Each task is independently reviewable and revertible; never bundle concurrency, input semantics, and navigation in one diff. Guard adoption and navigation semantics ship in separate commits so each reverts without touching the other.
- Corrected INV-I1 rule is normative: two settle samples commit **level** (`currentState` / `isPressed`); edge masks (`pressedEvents` / `releasedEvents`) are **only** required clear after the first subsequent normal loop-owned `gpio.update()`.
- Render ownership: exactly one submitting context per activity instance. For task-owned activities the display task is the sole submitter; boot never submits — it waits on the owning task's first-paint-complete signal. Global guard before any local mutex, never reverse. Display tasks use snapshot-under-local then render-plus-submit-under-global; synchronous main-task submit paths take global-then-local in order. No path takes local-then-global. Nothing adds, removes, or reorders a submit call; Phase 1 only serializes them.
- Boot-only blocking primitive (`waitForFirstPaint`, fulfilling the spec's `requestUpdateAndWait` role with CrossMux-identical misuse rules): exactly one caller (the boot first-paint path), bounded wait exactly 2 s, never from a render task or while holding the guard (assert in debug, serial diagnostic + immediate return in release), and exactly three allowed new boot serial lines (`[MAIN] First paint wait ok`, `[MAIN] First paint wait timeout, proceed to settle`, `[MAIN] Input settle done`). No existing serial line may change format. Timeout abandons the wait and proceeds to settle — the owning task keeps sole ownership, so single ownership survives the failure path and boot never halts.
- Navigator: `NavNext`/`NavPrevious` appended after `PageForward` (ordinals 0–8 unchanged, 11 enumerators total fit `uint16_t`). Edge-level equivalence resolved by recursion into synth-inclusive member getters placed BEFORE synthetic-Back/synthetic-Key/footer branches, plus a member-expansion synth matcher so injected Down/Right satisfies `NavNext` and Up/Left satisfies `NavPrevious`; `isPressed` stays level-OR. Portrait policy, no orientation swap. AppList Left/Right stay explicit (install/uninstall); vertical moves use explicit-axis navigator overloads, not logical buttons.
- Suppression: physical-only state that survives `beginFrame()` untouched; synthetic edges neither set, clear, nor observe suppression. Consumed frames skip activity/subactivity dispatch re-reads only: global gesture routing already executed above the check is not undone, and frame-tail bookkeeping (frontlight re-apply, deferred-delete drain, loop stats, `delay(10)`/`yield()`) still runs. Held levels remain visible next frame; render tasks unaffected.
- Exclusions (must not enter in any task): synthetic FIFO/`InteractionBuffer` of any kind; one-shot synthetic semantics / 40 ms gate / `beginFrame` clear changes; process-wide render-dispatch flag / central task / shared dispatcher / central ActivityManager; wholesale `ActivityManager` port (stack, pending actions, `goTo…`, MainTab, exclusive storage); new public render methods on any activity; font work; touch-classifier widening (wt-touch-p0 seam untouched); plugin changes; static memory estimates / governance refusal; SDK sync (`firmware/open-m4-sdk`, `firmware/lib` untouched); broad `ActivityManager` port; QEMU input semantic changes; removal of any legacy path in the same diff as its replacement.
- Test executability rule (no host-stub harness exists in this repo — all 73 `firmware/tests/native_app/` tests compile dependency-free headers with bare `g++`, sometimes `-Ifirmware/src`; no test includes Arduino/FreeRTOS headers): behavioral host tests may only cover dependency-free pure helpers that real production code consumes; FreeRTOS/Arduino-bound behavior is gated by Python source contracts plus named QEMU/`m4adb` behavior gates. No tautological model-only tests. Every RED gate states why it fails for the intended missing behavior.
- No behavior flips without its invariant RED gate passing GREEN first. QEMU/`m4adb` suites run unchanged as regression gates. Allocation behavior and heap baselines unchanged.
- Do not flash hardware. Do not push. Commit the plan doc only on this planning branch; implementation commits go on the implementation branch in small frequent units (one RED commit + one minimal GREEN commit per item minimum).

## Context And Current Facts (mapped 2026-09-11, read-only)

- `Activity` public API is `onEnter`/`onExit`/`loop` only (`firmware/src/activities/Activity.h:28-50`); `render()` methods are private/protected on every activity. There is no public synchronous render entry anywhere — a main-task "run its render synchronously" call is not implementable without inventing one, which is excluded above. The spec-compatible first-paint mechanism is therefore the spec's second alternative: the owning display task submits, and boot blocks on its completion signal.
- `firmware/src/activities/apps/AppListActivity.cpp:157-173` `displayTaskLoop` reads `updateRequired_`/`subActivity` under `renderingMutex_` every 10 ms; `:271-284` `selectIndex`/`moveSelection` write `selectedIndex_`/`updateRequired_` with no lock; `:233-262` `reload`/`onEnter`/`onExit` create/take/delete `renderingMutex_`; `:316-345` `openSelected`/`openInstall`/`uninstallSelected` take the mutex and `openSelected`/`openInstall` call `enterNewActivity` while holding it; `:356+` `loop()` dispatches keys/touch/swipe; `:483-535` `render()` has the single submit `:535`, called only from `displayTaskLoop`. Header `AppListActivity.h:38-41` (`touchFooterButtonsMask` reads `mode_`/`selectedIsPlugin()`), `:60-78` (`DrawerItem` with `std::string`+`pluginIcon`, `apps_`/`items_` vectors, `selectedIndex_`, `updateRequired_`, `mode_`, `uninstallClearData_`, task handle, `renderingMutex_`). Grid constants `:34-35` (`kAppLongPressMs = 700`, `kDrawerColumns = 3`).
- Boot destinations and their owning tasks: `onGoHome()` → `HomeActivity` (`main.cpp:666-679`); reader branch → `ReaderActivity` (`main.cpp:543-547`) which synchronously enters a child in `ReaderActivity::onEnter` (`:291+`: `goToLibrary()` → `MyLibraryActivity`, or `EpubReaderActivity`/`TxtReaderActivity`/`XtcReaderActivity`). Every boot-reachable destination owns a display task started synchronously in its `onEnter`: `HomeActivityTask` + `HomeSceneBackend` (`HomeActivity.cpp:1141-1229`, seeds `updateRequired=true` before `xTaskCreate`), `MyLibraryActivityTask` (`MyLibraryActivity.cpp:383-419`), `EpubReaderActivityTask` (`EpubReaderActivity.cpp:274-278`, existing `APP_STATE.isRenderComplete=false→true` completion precedent around `renderScreen()` in `displayTaskLoop():1725-1748`), `TxtReaderActivityTask` (`TxtReaderActivity.cpp:328-449`, loop `:2068`), `XtcReaderActivityTask` (`XtcReaderActivity.cpp:43-84`). Home already tracks `firstRenderDone` (`HomeActivity.h:47`, reset `:1150`, read `:1754`, set after first submit `:1830-1831`) — the natural first-paint-complete signal to promote to atomic + accessor.
- Submit-site classification (every direct `displayBuffer` that must acquire the global guard): AppList task-owned only — `render():535`. Home task-owned only — `renderSnapshotScene():872,:880,:893`, `renderMemWarning():1711`, `render():1747,:1827`, all reached solely via `displayTaskLoop():1636-1656` (`render()` has no other caller; `loop():1402+` only sets `updateRequired` flags). MyLibrary mixed — task-owned `render():1073` via `displayTaskLoop():962-970` (skips while `isPreviewingImage`), plus synchronous main-task submits from `loop():437+` paths: indexing popup `:507`, gray-preview/normalize passes `:559,:610,:712,:725`, action-menu `:883`, `executeActionMenu():1107`, `previewImage():1250,:1289,:1327,:1348`, `drawPreviewImageMenu():1633`. Settings task-owned only — `render():771` via `displayTaskLoop():719-729` (Settings guard adoption is out of Phase 1; its render path stays as-is).
- `firmware/src/main.cpp:122` owns `currentActivity`; `:412-430` lifecycle trio; `:478-482` `waitForPowerRelease` (power-only loop); `:709+` `setup()`; `:1250-1269` home-or-reader branch pinned by `[MAIN] home1` / `[MAIN] reader`; setup tail calls `waitForPowerRelease()`; `:1281+` `loop()` runs `gpio.update()` → NTP state machine → power/deep-sleep checks → `:1517-1529` long-press Back-to-home (level + `getHeldTime() >= 1500`, `longPressBackHomeFired` latch) → `mappedInputManager.beginFrame()` → bridge poll → gesture routing (home/history/back-gesture branches, each `return`-terminated) → `currentActivity->loop()` → frontlight re-apply → deferred-delete drain → loop stats → `delay(10)`/`yield()` (`:1580-1660` frame-tail bookkeeping that a consumed frame must still run).
- `firmware/src/MappedInputManager.h:10` 9-value `Button` enum; `:29-32` `beginFrame`/`wasPressed`/`wasReleased`/`isPressed`; `:70-81` synthetic slot; `:118` `kSynthMinIntervalMs = 40`. `MappedInputManager.cpp:36-80` `beginFrame` clears synth + tap/swipe caches + `syntheticBack` + touch-held override (suppression state must be added outside this clear); `:133-148` `injectSyntheticKey` one-shot + rate gate; `:149-175` `mapButton`; `:177-183` `wasPressed` (syntheticBack-Back check, then exact `synthKey_ == button`, then `mapButton`); `:185-236` `wasReleased` (same Back/synth checks, then footer-tap Back/Confirm/Left/Right branch, then `mapButton`); `:238` `isPressed` (`mapButton` only — synthetic Keys never affect level). Consequence: exact-match `synthKey_ == button` can never satisfy a `NavNext` query from an injected Down — recursion into member getters plus member-expansion synth matching is required (see Task 6).
- `firmware/src/util/ButtonNavigator.h:47-52` `getNextButtons() → {Down, Right}`, `getPreviousButtons() → {Up, Left}`; helpers `onNext/Previous(Press|Release|Continuous)`, generic `onPress/onRelease/onContinuous(Buttons, cb)`, index/page math. `ButtonNavigator.cpp` implements press as any-`wasPressed`, release as any-`wasReleased` gated by `lastContinuousNavTime`, continuous as any-`isPressed` + timing.
- Debounce source `firmware/open-m4-sdk/libs/hardware/InputManager/include/InputManager.h:242,244-245` (`currentState`, `pressedEvents`, `releasedEvents`); `:298` `DEBOUNCE_DELAY = 5`; `:24,27,30,36,50` `update()` / `isPressed` / `wasPressed` / `wasReleased` / `getHeldTime` (read-only; SDK not touched).
- Per-site navigation regions: AppList grid release edges `AppListActivity.cpp:460-478` (Up/Down ±`kDrawerColumns`, Left uninstall-dialog plugin-gated, Right install, Confirm open, Back back); MyLibrary rows `MyLibraryActivity.cpp:680-756` (release edges: Up-or-Left previous / Down-or-Right next; preview/action-menu rows keep menu-index behavior); Home press edges `HomeActivity.cpp:810-834` (Up-or-Left previous / Down-or-Right next pressed, Confirm released, Back pressed early-return); Settings exact enumeration — Back pressed `:194`, Confirm released `:291`, Hub pane `:303-316` (up||left → `settingsNavMoveHub(-1)`, down||right → `settingsNavMoveHub(+1)`), Row pane `:317-330` (up||left → `settingsNavMoveRow(-1)` + `settingsNavSyncWindow`, down||right → `(+1)`). These 6 references are the complete `Button::` enumeration in `SettingsActivity.cpp`; all other `settings/*` activities stay unmigrated in Phase 1.
- Baseline gates (run unchanged; not run during planning): host precedent `g++ -std=c++20 firmware/tests/native_app/test_m4_touch_wifi_keyboard_contract.cpp -o "$RUNNER_TEMP/…"` (`.github/workflows/m4-fast.yml:76-87`), `g++ -std=c++17 -Wall -Wextra -Werror -Ifirmware/src firmware/tests/native_app/test_progressive_http_state.cpp` (`.github/workflows/m4-progressive-stream.yml:43`), `python3 firmware/tests/test_m4_touch_wifi_ui_contract.py`, `python3 firmware/tests/test_m4_dependency_bootstrap_contract.py`, `tests/contracts/memory_governance_contracts_red.cpp`, `simulator/tests/test_qemu_boot_probe.py` (unit-level `probe_boot.classify` checks), `simulator/tests/synth_input_burst_e2e.py`, `./m4sim test smoke|network-manager|reader-ui --plugin-debug --skip-build --ready-seconds 90`.

### Task 1: INV-R1 RED — guarded-emission source contracts + gate refs (no behavior change)

**Files:**
- Create: `firmware/tests/test_phase1_r1_guard_contract.py`

**Interfaces:**
- Consumes: `AppListActivity.h:60-78`, `AppListActivity.cpp:160-173,271-284,316-345`, `main.cpp:122`, `M4RenderGuard.h` (absent → RED).
- Produces: failing contract pinning (a) `M4RenderGuard.h` exists with move-forbidden RAII + explicit `unlock` + static `peek` + bounded-wait acquire (no `portMAX_DELAY` on new paths); (b) `main.cpp` owns one process-wide mutex alongside `currentActivity`; (c) `selectIndex`/`moveSelection` bodies take the local mutex; (d) `displayTaskLoop` implements snapshot-under-local → release → render-plus-submit-under-global with `updateRequired_`/`subActivity` rechecked under local; (e) `openSelected`/`openInstall` release the local mutex before `enterNewActivity`; (f) per-activity submit-site guard lists for AppList/Home/MyLibrary (see Task 2/8).
- No `firmware/tests/native_app/*.cpp` in this task: FreeRTOS `SemaphoreHandle_t`/`xTaskCreate` cannot compile under the repo's bare-`g++` host harness (no Arduino/FreeRTOS stub exists — verified: zero stub/mock headers under `firmware/tests/` and `simulator/native/`, zero `Arduino.h`/`FreeRTOS` includes across all 73 host tests). A `std::thread` model of the discipline would assert only its own model, so it is excluded as tautological. Concurrency evidence comes from the source contract plus the Task 11 QEMU AppList browse/select journey and full-suite serial-anomaly rule.

- [ ] **Step 1: Write the failing source contract** asserting (a)–(f) above.

- [ ] **Step 2: RED commit and verify failure**

  Command:

  ```bash
  python3 firmware/tests/test_phase1_r1_guard_contract.py
  ```

  Expected: fails (missing `M4RenderGuard.h`, unlocked `selectIndex` writes, single-phase `displayTaskLoop`, enter-under-lock in `openSelected`/`openInstall`). Non-zero exit is the RED gate — it fails because the guard, the two-phase pattern, and the release-before-enter do not exist yet.

- [ ] **Step 3: Commit RED only**

  `git add firmware/tests/test_phase1_r1_guard_contract.py && git commit -m "test(phase1-r1): RED guarded-emission contract"`

### Task 2: Item 1 GREEN (part A) — M4RenderGuard + global mutex + AppList adoption (INV-R1)

**Files:**
- Create: `firmware/src/util/M4RenderGuard.h` (new; only new header this task; FreeRTOS includes only — never included by host tests)
- Modify: `firmware/src/main.cpp` (add the process-wide mutex alongside `currentActivity` `:122` only; the wait primitive itself lands in Task 4)
- Modify: `firmware/src/activities/apps/AppListActivity.cpp` + `AppListActivity.h` (snapshot struct, guarded writers, two-phase loop, release-before-enter)

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
  ```

  Misuse: never acquired while holding any local mutex (global-before-local everywhere); bounded waits only on new paths; no new `portMAX_DELAY`.
- AppList snapshot (dirty-frames only, so vector copies never sit on the 10 ms poll path):

  ```cpp
  struct AppListFrameSnapshot {
    int selectedIndex = 0;
    int mode = 0;
    bool uninstallClearData = true;
    std::vector<DrawerItem> items;
    std::vector<M4xInstalledApp> apps;
  };
  ```

  Indices alone are insufficient because `reload()` (`:177-242`) replaces `items_`/`apps_` wholesale — a render reading members after releasing the lock could mix generations (torn drawer). The drawer is small (8 builtins + installed apps), so a dirty-only deep copy is cheaper than a shared-pointer refactor here. `render()` keeps its signature and reads from a `const AppListFrameSnapshot&` (members stay the backing store for `loop()` hit-testing; the snapshot is the submit path's input).

- [ ] **Step 1: Add `M4RenderGuard` verbatim-port (name + owner only).** No policy logic inside the guard.

- [ ] **Step 2: Add the process-wide mutex in `main.cpp`.** No waiter, no dispatch flag, no central task.

- [ ] **Step 3: Minimal AppList adoption (highest traffic + confirmed race only).** Take only the local mutex in `selectIndex` (`:271`) / `moveSelection` (`:278`); keep math and the `setMask` policy calls identical. Rework `displayTaskLoop` (`:160-173`): take local, copy snapshot + recheck `updateRequired_ && !subActivity` (keep the existing stale-observation comment), clear `updateRequired_`, release local, then take the global guard and render-from-snapshot plus submit (`render():535`, the sole AppList submit). Rewrite `openSelected` (`:316-335`) / `openInstall` (`:336-343`): copy the needed item/app data to locals under the local mutex, release it, then `enterNewActivity` — never hold local across child installation, so no local→global nesting is possible.

- [ ] **Step 4: GREEN verification + commit**

  Commands:

  ```bash
  python3 firmware/tests/test_phase1_r1_guard_contract.py
  python3 firmware/tests/test_m4_touch_wifi_ui_contract.py
  ```

  Expected: both pass (exit 0). Then `git commit -m "feat(phase1-r1): guard AppList selection + two-phase submit"` — reverting restores unlocked flags/single-phase loop/enter-under-lock and deletes the shared mutex with no other item touched.

### Task 3: INV-I1 RED — settle + boot-order source contract + QEMU gate ref (no behavior change)

**Files:**
- Create: `firmware/tests/test_phase1_i1_boot_order_contract.py`

**Interfaces:**
- Consumes: `main.cpp:1250-1269` (home-or-reader branch), setup tail `waitForPowerRelease()` (`:478-482`), `loop():1281+` first `gpio.update()`, `InputManager.h:242,244-245,298`, `MappedInputManager.cpp:133-148` (synthetic one-shot, must stay untouched).
- Produces: failing contract pinning the corrected rule — (a) a first-paint completion wait exists after the home/reader branch and before `waitForPowerRelease()`; (b) two `gpio.update()` samples spaced `delay(10)` follow the wait; (c) no code asserts or forces `pressedEvents`/`releasedEvents` clear at the settle site — the masks are required clear only after the first subsequent normal loop-owned `gpio.update()`; (d) the three serial markers exist and no other serial format changed; (e) synthetic injection path and `beginFrame` clear keep exact semantics.
- No host sampler test: the debounced sampler lives in the untouched SDK (`InputManager.h`) behind `HalGPIO`, which cannot compile under bare `g++`. A host re-implementation of debounce would assert only its own model. Behavior evidence is the Task 11 QEMU boot-log gate (paint → settle → power-release order, markers, silent first frames). RED fails because the wait, the settle step, and the markers are absent.

- [ ] **Step 1: Write the failing boot-order source contract** asserting (a)–(e).

- [ ] **Step 2: RED commit and verify failure**

  Command:

  ```bash
  python3 firmware/tests/test_phase1_i1_boot_order_contract.py
  ```

  Expected: fails (no wait/settle/markers; boot still enter-then-wait-for-release). Non-zero exit is the RED gate.

- [ ] **Step 3: Commit RED only**

  `git add firmware/tests/test_phase1_i1_boot_order_contract.py && git commit -m "test(phase1-i1): RED settle + boot-order contract"`

### Task 4: Item 1 GREEN (part B) + Item 2 GREEN — first-paint completion wait + two-sample settle (INV-R1/INV-I1)

**Files (all required — a main.cpp-only change cannot gate the owning tasks, so it is invalid):**
- Modify: `firmware/src/main.cpp` (wait primitive + single boot call site + settle step + three serial lines)
- Modify: `firmware/src/activities/home/HomeActivity.h` + `HomeActivity.cpp` (promote `firstRenderDone` `:47` to `std::atomic<bool>`, add `bool firstPaintComplete() const` accessor, set it after the first guarded `displayBuffer` in the `render()` submit paths `:1747,:1827` and snapshot paths `:872,:880,:893`)
- Modify: `firmware/src/activities/reader/EpubReaderActivity.h` + `EpubReaderActivity.cpp` (add `std::atomic<bool> firstPaintComplete_` + accessor alongside the existing `APP_STATE.isRenderComplete` pattern; set after the first submit in `displayTaskLoop():1725+`)
- Modify: `firmware/src/activities/reader/TxtReaderActivity.h` + `TxtReaderActivity.cpp` (same flag in `displayTaskLoop():2068+`)
- Modify: `firmware/src/activities/reader/XtcReaderActivity.h` + `XtcReaderActivity.cpp` (same flag in its display-task submit path `:40+`)
- Modify: `firmware/src/activities/home/MyLibraryActivity.h` + `MyLibraryActivity.cpp` (same flag in `displayTaskLoop():962+`; covers boot-via-`goToLibrary`)
- Modify: `firmware/src/activities/reader/ReaderActivity.h` + `ReaderActivity.cpp` (forwarding accessor only — returns the current child's flag when the child offers one, `false` otherwise; no render changes, no new submit)

**Interfaces:**
- Produces (replaces the unimplementable "run its render synchronously on the main task" reading with the spec's second alternative — task-gated first paint — while keeping the primitive, timeout, assertions, and diagnostics the spec pins):

  ```cpp
  // main.cpp — boot-only blocking primitive fulfilling the requestUpdateAndWait role.
  // Polls firstPaintComplete() on the boot destination's owning task; boot submits nothing.
  bool waitForFirstPaint(TickType_t timeout = pdMS_TO_TICKS(2000));
  ```

  Misuse (CrossMux-identical intent, adapted to per-activity tasks): never from any display task and never while holding the global guard — debug assert via `M4RenderGuard::peek` + setup-only call-site restriction; release builds emit the serial diagnostic and return `false` immediately. Boot sequence: enter initial activity (starts the owning task synchronously in `onEnter`) → `waitForFirstPaint()` → two-sample settle → `waitForPowerRelease()` → loop. Exactly one context submits the first frame (the owning task); the timed-out wait is abandoned, never retried alongside a new submitter. No per-frame caller, no global dispatch flag, no central ActivityManager, no new public render method on any activity.

- [ ] **Step 1: Add first-paint-complete flags to the six owning activities + Reader forwarding.** Flags are set-once (first guarded submit only), read with acquire ordering, cleared on `onExit`.

- [ ] **Step 2: Add the single boot call site in `main.cpp`.** After the home-or-reader branch (`:1250-1269`), before `waitForPowerRelease()`. Home branch polls the `HomeActivity` flag; reader branch polls the `ReaderActivity` forwarding accessor. Success emits `[MAIN] First paint wait ok`; timeout emits `[MAIN] First paint wait timeout, proceed to settle` and proceeds (covers synchronous-path destinations whose first submit lands in early `loop()` frames — settle still guarantees silent edges because level persists while held and edges clear before first dispatch).

- [ ] **Step 3: Add the two-sample settle (corrected sequencing).** Two `gpio.update()` samples spaced `delay(10)` (20 ms wall, boot-only). Level commits; edge masks stay latched unread until the first loop-owned `gpio.update()` clears them. Settle completion emits `[MAIN] Input settle done`. Synthetic path untouched by construction.

- [ ] **Step 4: GREEN verification + commit**

  Commands:

  ```bash
  python3 firmware/tests/test_phase1_i1_boot_order_contract.py
  python3 firmware/tests/test_phase1_r1_guard_contract.py
  python3 firmware/tests/test_m4_touch_wifi_ui_contract.py
  ```

  Expected: all pass. Then `git commit -m "feat(phase1-i1): first-paint completion wait + two-sample settle"` — reverting deletes the flags, the single boot call site, and the settle step; boot returns to enter-then-wait-for-release with no other item touched. QEMU boot-log proof is Task 11.

### Task 5: INV-N1 RED — behavioral mapping test + adoption contract (no behavior change)

**Files:**
- Create: `firmware/tests/native_app/test_phase1_n1_nav_mapping.cpp`
- Create: `firmware/tests/test_phase1_n1_adoption_contract.py`

**Interfaces:**
- Consumes: `MappedInputManager.h:10` enum, `:177-238` getters + synthetic/footer branches, `ButtonNavigator.h:47-52`, per-site regions (AppList `:460-478`, MyLibrary `:680-756`, Home `:810-834`, Settings `:194,:291,:303-330`).
- Produces: (a) a REAL behavioral host test over `util/M4NavMapping.h` — the dependency-free helper production consumes (see Task 6) — proving injected Down/Right satisfy `NavNext` and Up/Left satisfy `NavPrevious` through the actual member-expansion + synth-match logic, plus full 11-button × edge equivalence and `isPressed` level-OR; (b) a source contract pinning enum append-after-`PageForward` (ordinals 0–8 unchanged), recursion-before-synth/footer placement in `wasPressed`/`wasReleased`, singleton getters, index/page math untouched, and the per-site adoption table.
- The host test compiles with the repo's established pure-header pattern (`g++ -std=c++20 -Ifirmware/src`, cf. `test_progressive_http_state`), so it exercises production logic — not grep. RED fails for the intended reasons: before Task 6 the helper header does not exist (compile error), and even with the helper present the production-wiring assertions fail (no `NavNext` branch in `wasPressed`, raw Down/Right getter lists).

- [ ] **Step 1: Write the failing behavioral test** — synthetic Down/Right/Up/Left through `m4SynthSatisfiesKey` + member-OR composition; every button × {pressed, released, level} logical-vs-legacy equivalence; direct `NavNext` injection also satisfied; `uint16_t` mask capacity for 11 enumerators.

- [ ] **Step 2: Write the failing adoption source contract** — enum append, recursion placement, singleton getters, AppList explicit-axis verticals (never logical for Left/Right), MyLibrary/Home/Settings rows per Task 6–7 table.

- [ ] **Step 3: RED commit and verify failure**

  Commands:

  ```bash
  g++ -std=c++20 -Ifirmware/src firmware/tests/native_app/test_phase1_n1_nav_mapping.cpp -o /tmp/phase1_n1_nav && /tmp/phase1_n1_nav
  python3 firmware/tests/test_phase1_n1_adoption_contract.py
  ```

  Expected: the compile fails (no helper yet) and the contract fails (no enum/wiring). Both non-zero — the RED gate.

- [ ] **Step 4: Commit RED only**

  `git add firmware/tests/native_app/test_phase1_n1_nav_mapping.cpp firmware/tests/test_phase1_n1_adoption_contract.py && git commit -m "test(phase1-n1): RED logical-navigation gates"`

### Task 6: Item 3 GREEN (core + AppList) — mapping helper + enum + recursion + AppList adoption (INV-N1)

**Files:**
- Create: `firmware/src/util/M4NavMapping.h` (new; dependency-free: `<cstdint>` only — zero Arduino/FreeRTOS includes — so both production and the Task 5 host test consume it)
- Modify: `firmware/src/MappedInputManager.h` (enum append only) + `firmware/src/MappedInputManager.cpp` (recursion + synth matcher wiring)
- Modify: `firmware/src/util/ButtonNavigator.h` (`getNextButtons` → `{NavNext}`, `getPreviousButtons` → `{NavPrevious}` only)
- Modify: `firmware/src/activities/apps/AppListActivity.cpp:460-478` (explicit-axis navigator adoption only; no other logic)

**Interfaces:**
- Produces:

  ```cpp
  // firmware/src/util/M4NavMapping.h — pure mapping truth, portrait policy, no orientation swap.
  enum class M4NavButton : uint8_t { Back=0, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward, NavNext, NavPrevious };
  [[nodiscard]] constexpr bool m4IsNavNextMember(M4NavButton b);      // Down || Right
  [[nodiscard]] constexpr bool m4IsNavPreviousMember(M4NavButton b);  // Up || Left
  // Member-expansion synth matcher: an injected member key satisfies its logical query.
  [[nodiscard]] constexpr bool m4SynthSatisfiesKey(M4NavButton synthKey, M4NavButton query);
  // Tri-state edge fold over already-resolved member edges (each synth-inclusive at the call site):
  [[nodiscard]] constexpr bool m4NavNextEdge(bool downEdge, bool rightEdge);
  [[nodiscard]] constexpr bool m4NavPreviousEdge(bool upEdge, bool leftEdge);
  [[nodiscard]] constexpr bool m4NavNextLevel(bool downLevel, bool rightLevel);
  [[nodiscard]] constexpr bool m4NavPreviousLevel(bool upLevel, bool leftLevel);
  ```

  ```cpp
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward, NavNext, NavPrevious };
  static_assert((int)Button::NavNext == (int)M4NavButton::NavNext);  // ordinals pinned, 0–8 unchanged
  ```

  Wiring in `wasPressed`/`wasReleased` (BEFORE the syntheticBack, synthetic-Key, and footer-tap branches — this ordering is the fix; the old plan's claim that `synthKey_ == NavNext` handles member injections was false because injection stores the member key, never the logical one):

  ```cpp
  bool MappedInputManager::wasPressed(Button b) const {
    if (b == Button::NavNext) return wasPressed(Button::Down) || wasPressed(Button::Right);
    if (b == Button::NavPrevious) return wasPressed(Button::Up) || wasPressed(Button::Left);
    if (b == Button::Back && syntheticBack) return true;
    if (synthKind_ == SynthKind::Key && m4SynthSatisfiesKey((M4NavButton)synthKey_, (M4NavButton)b)) return true;
    ...footer-tap branch (wasReleased only, unchanged eligibility)...
    return mapButton(b, &HalGPIO::wasPressed);  // + NavNext/NavPrevious cases rejected here (unreachable)
  }
  bool MappedInputManager::isPressed(Button b) const {
    if (b == Button::NavNext) return isPressed(Button::Down) || isPressed(Button::Right);
    if (b == Button::NavPrevious) return isPressed(Button::Up) || isPressed(Button::Left);
    return mapButton(b, &HalGPIO::isPressed);  // level-OR; synthetic Keys never affect level (unchanged)
  }
  ```

  Recursion terminates (members are never logical) and each member call is synth-inclusive, so injected Down/Right satisfies `NavNext` on both edges exactly as it reads as Down. `mapButton` gains no `NavNext` case (any reach is a bug — assert/unreachable). Continuous navigation flows through `isPressed` level-OR unchanged. Eleven enumerators fit `uint16_t` masks.
- AppList adoption (first per migration order; release-edge kind preserved): vertical moves via explicit-axis overloads `onRelease({Down})` → `moveSelection(+kDrawerColumns)`, `onRelease({Up})` → `moveSelection(-kDrawerColumns)`; Left (uninstall dialog, plugin-gated), Right (install), Confirm (open), Back (back) keep exact bindings through navigator helpers. Swipe paths (`:449-452`) and touch paths unchanged.

- [ ] **Step 1: Add `M4NavMapping.h` + enum append + static_asserts.** Helper compiles standalone under `g++ -std=c++20 -Ifirmware/src` with `-Wall -Wextra -Werror`.

- [ ] **Step 2: Wire recursion + `m4SynthSatisfiesKey` into the three getters** in the exact order above. Footer-tap eligibility and synthetic-Back paths untouched.

- [ ] **Step 3: Flip the two getters to logical singletons + AppList explicit-axis cutover.** No other `ButtonNavigator` change; index/page math untouched.

- [ ] **Step 4: GREEN verification + commit (navigation-only; guard untouched)**

  Commands:

  ```bash
  g++ -std=c++20 -Wall -Wextra -Werror -Ifirmware/src firmware/tests/native_app/test_phase1_n1_nav_mapping.cpp -o /tmp/phase1_n1_nav && /tmp/phase1_n1_nav
  python3 firmware/tests/test_phase1_n1_adoption_contract.py
  python3 firmware/tests/test_phase1_r1_guard_contract.py
  ```

  Expected: all pass. Then `git commit -m "feat(phase1-n1): logical mapping + AppList adoption"` — reverting the two getters to raw lists restores prior dispatch; appended enumerators + helper stay inert. MyLibrary/Home/Settings adoption is Task 7 (not here).

### Task 7: Item 3 GREEN (remainder, single-sourced) — MyLibrary + Home + Settings navigator adoption (INV-N1)

**Files (navigation dispatch only; no locking changes — guard adoption is Task 8):**
- Modify: `firmware/src/activities/home/MyLibraryActivity.cpp:680-756` (linear rows only; preview/action-menu rows keep menu-index behavior)
- Modify: `firmware/src/activities/home/HomeActivity.cpp:810-834` (lists only; Confirm/Back unchanged)
- Modify: `firmware/src/activities/settings/SettingsActivity.cpp:303-330` (two exact sites below; `:194` Back and `:291` Confirm unchanged)

**Interfaces:**
- Consumes: Task 6 mapping + getters.
- Produces, in migration order (AppList already done in Task 6):
  - MyLibrary (second): linear rows take `onNextRelease` / `onPreviousRelease` logical. Edge kind (release) preserved; identical behavior under the portrait policy.
  - Home (third): lists take `onNextPress` / `onPreviousPress` logical. Press-edge kind preserved; `backendCtx->updateRequired` / `updateRequired` wake paths unchanged.
  - Settings (exact enumeration — there are no other `Button::` rows in this file): Site S1 Hub pane (`:303-316`): up||left → `settingsNavMoveHub(-1)`, down||right → `settingsNavMoveHub(+1)` — both pairs are pure previous/next (no distinct Left/Right action), so convert to `onPreviousRelease` / `onNextRelease` logical. Site S2 Row pane (`:317-330`): up||left → `settingsNavMoveRow(-1)` + `settingsNavSyncWindow`, down||right → `(+1)` — likewise pure, convert to logical release overloads. Back (`:194`) and Confirm (`:291`) unchanged. All other `settings/*` activities stay unmigrated (behave exactly as today).

- [ ] **Step 1: MyLibrary logical release cutover.** Commit: `git commit -m "feat(phase1-n1): MyLibrary logical navigation"`.

- [ ] **Step 2: Home logical press cutover.** Commit: `git commit -m "feat(phase1-n1): Home logical navigation"`.

- [ ] **Step 3: Settings S1+S2 logical release cutover.** Commit: `git commit -m "feat(phase1-n1): Settings logical navigation"`.

- [ ] **Step 4: Verify all three (each commit independently revertible; none touches locking)**

  Commands:

  ```bash
  g++ -std=c++20 -Wall -Wextra -Werror -Ifirmware/src firmware/tests/native_app/test_phase1_n1_nav_mapping.cpp -o /tmp/phase1_n1_nav && /tmp/phase1_n1_nav
  python3 firmware/tests/test_phase1_n1_adoption_contract.py
  ```

  Expected: all pass after each commit.

### Task 8: Item 1 GREEN (remainder) — MyLibrary + Home guard adoption (INV-R1)

**Files (locking only; no dispatch changes — navigation is Tasks 6–7):**
- Modify: `firmware/src/activities/home/MyLibraryActivity.h:15-16,19,33` + `MyLibraryActivity.cpp` (snapshot discipline + submit-site guards)
- Modify: `firmware/src/activities/home/HomeActivity.h:24,32,42,44,78` + `HomeActivity.cpp` (snapshot discipline + submit-site guards; backend wake mechanisms unchanged)

**Interfaces:**
- Consumes: `M4RenderGuard` + global mutex from Task 2.
- Produces per-activity two-phase discipline with exact snapshot data:
  - MyLibrary: render consumes `selectorIndex`, `basepath`, view/preview/menu flags, plus file-list vectors (`files`/`fileSizes`, `searchResults`/`searchResultSizes`) that `loadFiles()`/`executeSearch()` replace wholesale — so indices alone are insufficient (same torn-generation hazard as AppList, with larger lists). Minimal-churn pattern: introduce `std::shared_ptr<const FileListSnapshot>` snapshots swapped under the local mutex by the loaders (cheap pointer swap, no per-frame deep copy); `displayTaskLoop` pins the pointer + copies scalars under local, releases local, renders-from-snapshot under the global guard. Global guard wraps every submit: task-owned `render():1073` plus the synchronous main-task submits `loop()` indexing popup `:507`, normalize/gray passes `:559,:610,:712,:725`, action-menu `:883`, `executeActionMenu():1107`, `previewImage():1250,:1289,:1327,:1348`, `drawPreviewImageMenu():1633` (each takes global-then-local in order; existing local takes at e.g. `:504-508` are reordered to global-first). Temporal exclusion (`!isPreviewingImage` task skip, preview/menu flags) is read and written under the local mutex so exactly one context submits at a time.
  - Home: `render()` reads publication (already pinned via `acquirePublication()` in `renderSnapshotScene()`), `sceneFocusIndex`, `selectorIndex`, cover/buffer flags, mem-warning flags. Snapshot = pinned publication + scalar copies taken under the local mutex; submits `:872,:880,:893,:1711,:1747,:1827` all run under the global guard via the task path (Home has no synchronous submits — `loop()` only sets `updateRequired` flags). Backend scene tasks keep their wake conditions (`backendCtx->updateRequired` / `updateRequired`); only submit serialization is added.
  - `ActivityWithSubactivity` display paths keep today's skip/defer behavior with guard acquisitions added around the existing submit calls, never new submits. Unmigrated activities behave exactly as today.

- [ ] **Step 1: MyLibrary snapshot + submit-site guards.** Commit: `git commit -m "feat(phase1-r1): MyLibrary render guard"`.

- [ ] **Step 2: Home snapshot + submit-site guards.** Commit: `git commit -m "feat(phase1-r1): Home render guard"`.

- [ ] **Step 3: Verify both (each commit independently revertible; neither touches dispatch)**

  Commands:

  ```bash
  python3 firmware/tests/test_phase1_r1_guard_contract.py
  g++ -std=c++20 -Wall -Wextra -Werror -Ifirmware/src firmware/tests/native_app/test_phase1_n1_nav_mapping.cpp -o /tmp/phase1_n1_nav && /tmp/phase1_n1_nav
  ```

  Expected: all pass after each commit (Task 8 gate).

### Task 9: INV-S1 RED — behavioral suppression test + frame-skip contract (no behavior change)

**Files:**
- Create: `firmware/tests/native_app/test_phase1_s1_suppress.cpp`
- Create: `firmware/tests/test_phase1_s1_frame_skip_contract.py`

**Interfaces:**
- Consumes: `main.cpp:1517-1529` Back site, `:1530-1580` gesture routing, `:1580-1660` frame-tail bookkeeping, `MappedInputManager.h:29-32,70-81`, `MappedInputManager.cpp:36-80` (`beginFrame` clear set).
- Produces: (a) a REAL behavioral host test over `util/M4SuppressState.h` — the dependency-free helper production consumes (see Task 10) — proving press → hold-past-threshold → release yields exactly one action with a consumed release, a subsequent fresh press fires again, and a mixed frame (unrelated edge + gesture pulse coincident with the consumed release) drops both while held level stays observable next frame; (b) a source contract pinning physical-only wiring (synthetic paths never reference suppression state), `beginFrame()` never touching the suppression mask, frame-top check placement between gesture routing and `currentActivity->loop()`, skip-to-frame-tail (never a bare return past deferred-delete/delay bookkeeping), and the Back 1.5 s site keeping threshold + destination with only firing/release-consumption changed.
- RED fails for the intended reasons: before Task 10 the helper header does not exist (compile error), there is no suppression state for `beginFrame` to preserve, no frame-top check exists, and the Back site still uses the ad-hoc `longPressBackHomeFired` latch.

- [ ] **Step 1: Write the failing behavioral test** over the planned `M4SuppressState` API (mask set/consume + one-shot threshold latch over plain edge/level/hold-time inputs — no Arduino types).

- [ ] **Step 2: Write the failing source contract** asserting helper existence + production wiring + `beginFrame`-untouched + placement + skip-to-tail + Back-site threshold/destination preservation.

- [ ] **Step 3: RED commit and verify failure**

  Commands:

  ```bash
  g++ -std=c++20 -Wall -Wextra -Werror -Ifirmware/src firmware/tests/native_app/test_phase1_s1_suppress.cpp -o /tmp/phase1_s1_suppress && /tmp/phase1_s1_suppress
  python3 firmware/tests/test_phase1_s1_frame_skip_contract.py
  ```

  Expected: the compile fails (no helper yet) and the contract fails (no state, no check, ad-hoc Back latch). Both non-zero — the RED gate.

- [ ] **Step 4: Commit RED only**

  `git add firmware/tests/native_app/test_phase1_s1_suppress.cpp firmware/tests/test_phase1_s1_frame_skip_contract.py && git commit -m "test(phase1-s1): RED consume-once + frame-skip gates"`

### Task 10: Item 4 GREEN — suppression state + frame-top check + one Back site (INV-S1)

**Files:**
- Create: `firmware/src/util/M4SuppressState.h` (new; dependency-free: `<cstdint>` only — consumed by both `MappedInputManager` and the Task 9 host test)
- Modify: `firmware/src/MappedInputManager.h` + `firmware/src/MappedInputManager.cpp` (hold one `M4SuppressState`; feed it from physical long-press/release paths only)
- Modify: `firmware/src/main.cpp` (frame-top check + single Back-site conversion only)

**Interfaces:**
- Produces (minimal, CrossMux-adapted minus BLE arrays):

  ```cpp
  // firmware/src/util/M4SuppressState.h — pure consume-once truth, no Arduino/FreeRTOS includes.
  struct M4SuppressState { uint16_t mask = 0; uint16_t firedLatch = 0; };
  [[nodiscard]] constexpr bool m4SuppressButtonIndex(uint8_t buttonIndex);  // 11 enumerators fit uint16_t
  constexpr void m4SuppressNextRelease(M4SuppressState&, uint8_t buttonIndex);
  // Frame-top fold over already-resolved physical release edges of this frame:
  constexpr bool m4ConsumeSuppressedRelease(M4SuppressState&, uint16_t physicalReleasedMask);
  // One-shot threshold event while held (replaces the ad-hoc already-fired latch;
  // sets latch + suppression once when isPressed && heldMs >= thresholdMs, clears when released):
  constexpr bool m4LongPressFired(M4SuppressState&, uint8_t buttonIndex, bool isPressed, unsigned long heldMs, unsigned long thresholdMs);
  ```

  Narrowed frame semantics (spec-pinned placement, accurately scoped): the check sits between gesture routing and `currentActivity->loop()`; when it reports consumed, the frame skips activity/subactivity dispatch re-reads (`currentActivity->loop()` call only) — it does NOT undo already-executed gesture routing (`wasHomeGesture`/`wasHistoryGesture`/`wasBackGesture` branches above it already ran), and it does NOT bare-return past frame-tail bookkeeping: frontlight re-apply, deferred-delete drain, loop stats, and `delay(10)`/`yield()` (`:1580-1660`) still run. Coincident unrelated edges/gesture pulses in that frame are dropped with it (not deferred/replayed); held levels stay observable next frame via normal `isPressed`; the next fresh press works normally; render tasks run independently. Converted site keeps the 1.5 s threshold and `onGoHome()` destination bit-for-bit; only firing (`m4LongPressFired`) plus release-consumption changes. `beginFrame()` keeps its exact clear set (synth + tap/swipe caches + `syntheticBack` + touch-held override) and never references the suppression mask — pinned by contract.

- [ ] **Step 1: Add `M4SuppressState.h` + physical-only wiring.** Synthetic-Key/footer-tap/`syntheticBack` paths never call the helpers (asserted by contract).

- [ ] **Step 2: Place the frame-top check with skip-to-frame-tail.** On consume, jump to the `:1580+` bookkeeping (frontlight, deferred-delete, stats, delay/yield), skipping only the `currentActivity->loop()` dispatch.

- [ ] **Step 3: Convert only the verified Back 1.5 s site (`:1517-1529`).** No other long-press site touched. Default the check behind a single toggle so rollback is a one-line default flip.

- [ ] **Step 4: GREEN verification + commit**

  Commands:

  ```bash
  g++ -std=c++20 -Wall -Wextra -Werror -Ifirmware/src firmware/tests/native_app/test_phase1_s1_suppress.cpp -o /tmp/phase1_s1_suppress && /tmp/phase1_s1_suppress
  python3 firmware/tests/test_phase1_s1_frame_skip_contract.py
  g++ -std=c++20 -Wall -Wextra -Werror -Ifirmware/src firmware/tests/native_app/test_phase1_n1_nav_mapping.cpp -o /tmp/phase1_n1_nav && /tmp/phase1_n1_nav
  ```

  Expected: all pass. Then `git commit -m "feat(phase1-s1): consume-once release + frame-top skip"` — reverting defaults the check off; Back site keeps level-read behavior.

### Task 11: Final integration/regression — PR70 touch/Wi-Fi + QEMU/m4adb gates (no new behavior)

**Files:**
- Modify: none (evidence task; failures here block the phase, they do not authorize scope expansion).

**Interfaces:**
- Consumes: all prior tasks GREEN.
- Produces: green gates proving behavior-identical except invariant fixes: PR70 touch/Wi-Fi contracts, full QEMU journey suite with strict serial-anomaly rule (no new lines, no changed formats — only the three named boot markers may appear as additions), synthetic one-shot journeys unchanged (`synth_input_burst_e2e`), boot-order proof via UART log (paint → settle → power-release with markers), memory-governance contracts untouched, no journey timing adjustments.

- [ ] **Step 1: Host + source contracts (fast gates first)**

  ```bash
  python3 firmware/tests/test_m4_dependency_bootstrap_contract.py
  python3 firmware/tests/test_m4_touch_wifi_ui_contract.py
  g++ -std=c++20 firmware/tests/native_app/test_m4_touch_wifi_keyboard_contract.cpp -o /tmp/touch-kb && /tmp/touch-kb
  python3 firmware/tests/test_m4_file_transfer_service_ownership_contract.py
  python3 firmware/tests/test_m4_esp_http_server_contract.py
  python3 firmware/tests/test_m4_navigation_supervisor_contract.py
  python3 firmware/tests/test_m4_memory_governance_contract.py
  python3 firmware/tests/test_phase1_r1_guard_contract.py
  python3 firmware/tests/test_phase1_i1_boot_order_contract.py
  g++ -std=c++20 -Wall -Wextra -Werror -Ifirmware/src firmware/tests/native_app/test_phase1_n1_nav_mapping.cpp -o /tmp/phase1_n1_nav && /tmp/phase1_n1_nav
  python3 firmware/tests/test_phase1_n1_adoption_contract.py
  g++ -std=c++20 -Wall -Wextra -Werror -Ifirmware/src firmware/tests/native_app/test_phase1_s1_suppress.cpp -o /tmp/phase1_s1_suppress && /tmp/phase1_s1_suppress
  python3 firmware/tests/test_phase1_s1_frame_skip_contract.py
  ```

  Expected: all pass (exit 0). Any failure stops the phase; fix inside the owning task, never by widening scope.

- [ ] **Step 2: QEMU/m4adb regression (unchanged suites + boot-order proof)**

  ```bash
  cd firmware && pio run -e murphy_m4_qemu_plugin -j 2
  ./m4sim test smoke --plugin-debug --skip-build --ready-seconds 90
  ./m4sim test network-manager --plugin-debug --skip-build --ready-seconds 90
  ./m4sim test reader-ui --plugin-debug --skip-build --ready-seconds 90
  python3 simulator/tests/synth_input_burst_e2e.py
  python3 simulator/tests/test_qemu_boot_probe.py
  ```

  Expected: green throughout; QEMU UART log shows guarded first paint → `[MAIN] First paint wait ok` (or the timeout marker on synchronous-path boots) → `[MAIN] Input settle done` → power-release wait, with only those markers added; MyLibrary down/confirm/back journey green without timing adjustments; long-press Back (home, 1.5 s) journey green with threshold unchanged; AppList grid vertical ±1 row and Left/Right install/uninstall bindings intact; synthetic one-shot semantics unchanged.

- [ ] **Step 3: Record evidence, no code change**

  Capture exact heads, CI run IDs, and boot-log excerpts in the Task 11 report. Commit nothing in this task unless updating the plan's evidence appendix is explicitly requested.

## Risks / Rollback

- Lock-order inversion (global-after-local somewhere) → deadlock. Mitigation: Tasks 2/8 enforce global-before-local by construction (including reordering MyLibrary's existing local takes at e.g. `:504-508` to global-first); RED contract pins order; bounded waits only on new paths.
- First-paint flag never set (task failed to start) → 2 s boot delay, then timeout path. Mitigation: bounded wait + proceed-to-settle; boot never halts; failure is diagnosable via the timeout marker.
- Edge-kind migration (press↔release) silently changing UX. Mitigation: Tasks 6–7 preserve edge kinds per site; AppList Left/Right never fold into logical.
- Suppression swallowing already-routed gestures. Mitigation: Task 10 scopes suppression to dispatch re-reads only — gesture routing above the check already ran and is never undone; skip-to-tail keeps bookkeeping; synthetic inert; default-off toggle gives one-line rollback.
- Per-item rollback (spec §Rollback boundaries, adapted to completion-wait): guard → remove acquisitions, restore flags, delete mutex; primitive → delete flags + single boot call site (boot returns to enter-then-wait-for-release); settle → delete two-sample step; navigator → revert two getters (enumerators + helpers inert); suppression → default check off. No legacy path is removed in the same diff as its replacement.

## Open Questions

None. All material choices are pinned by the spec tip `cf19660` and the mapped baselines above. If implementation uncovers a spec/plan conflict, halt that task and surface it before proceeding — the spec is authoritative.

## Plan self-review (pre-commit, revised)

- Spec coverage: Item 1 (Tasks 1–2 + 4-part-B + 8) covers RenderLock port, lock order, two-phase pattern with exact per-activity snapshot data and submit-site lists, misuse rules, 2 s bound, single owner incl. timeout — via the spec's task-gated alternative instead of the unimplementable synchronous-render reading (no public render entry exists; `Activity.h:28-50`). Item 2 (Tasks 3–4) covers corrected INV-I1 level-vs-edge rule, placement after home/reader decision, 10 ms spacing, 20 ms cost, synthetic unaffected, three serial markers, no-format-change. Item 3 (Tasks 5–7) covers 11-enumerator append, edge-level synthetic-inclusive equivalence via recursion-before-synth/footer + member-expansion matcher, portrait policy, singleton getters, per-site table with AppList explicit axes and exact Settings S1/S2 rows, INV-N1 acceptance (1)–(4). Item 4 (Tasks 9–10) covers physical-only suppression surviving `beginFrame`, `uint16_t` mask, Back 1.5 s site bit-for-bit threshold/destination, spec-pinned placement with narrowed skip-to-tail semantics, held-levels-next-frame, synthetic inertness, INV-S1 acceptance (1)–(4). Migration order (AppList → MyLibrary → Home) and TDD rhythm (RED before GREEN, QEMU/m4adb unchanged gates) preserved; guard vs navigation commits are independently revertible (Tasks 6–7 never touch locking; Task 8 never touches dispatch).
- Placeholders: none — every task names exact files, line regions, interfaces, commands, expected exits, and commit messages. Settings rows are exactly enumerated (S1 `:303-316`, S2 `:317-330`, Back `:194`, Confirm `:291`); no "remaining settings rows" language remains. Task 4 names all 13 files required to gate first paint.
- Interface consistency: `M4RenderGuard` (move-forbidden, `unlock`, `peek`) used identically in Tasks 1–2/4/8; `waitForFirstPaint(2000 ms)` single boot caller polling per-activity `firstPaintComplete()` flags; `M4NavButton` ordinals static-asserted against `Button` shared by Tasks 5–7/9–10 masks; `M4SuppressState` trio physical-only shared by Tasks 9–10; no global dispatch flag, FIFO, governance, font, touch, plugin, SDK, or central-ActivityManager surface introduced.
- Test executability: every host `.cpp` gate covers a dependency-free header with the repo's established bare-`g++` (`-Ifirmware/src` where needed) pattern — `M4NavMapping.h` / `M4SuppressState.h` (`<cstdint>` only) consumed by real production code; FreeRTOS/Arduino-bound behavior is gated by Python source contracts + named QEMU/`m4adb` journeys. Each RED states why it fails for the intended missing behavior. The old plan's bare-`g++` Arduino tests and tautological model-only tests are gone.
- Plan/spec conflicts found: one apparent conflict resolved — the spec's "run its render synchronously under guard on the main task" is not implementable under the existing `Activity` API (no public render entry) and would create the second render owner the spec forbids (the owning task starts synchronously in `onEnter` before any main-task render could run). The plan implements the spec's second alternative (task-gated first paint: boot blocks on the owning task's completion signal) which satisfies every pinned constraint: single submitter, blocking primitive with identical misuse rules/timeout/diagnostics, exactly one boot caller, no second owner on timeout, no global dispatch flag. The old plan's `synthKey_ == NavNext` auto-handling claim was false (injection stores member keys) and is replaced by recursion + member-expansion matching. The old plan's bare-return frame-skip is narrowed to skip-to-frame-tail per the spec's own data-flow language ("skips to end-of-loop bookkeeping").



