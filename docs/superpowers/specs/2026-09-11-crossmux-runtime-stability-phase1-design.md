# CrossMux-Derived Runtime Stability, Phase 1 — Design

## Context

First-stage stability layer for m4-stack, limited to memory/UI/stability. It ports
four proven CrossMux mechanisms (render-guard discipline, first-paint input
settle, logical navigation buttons, consume-once release suppression) into the
existing m4-stack frame without changing the top-level lifecycle, the synthetic
input path, or any rendering cadence.

Base: `github/ui/touch-wifi-keyboard@8317a89`
Design branch: `runtime/crossmux-stability-phase1-design`
CrossMux reference: `reference/crossmux` at `a67ca036` (freeink-sdk `8242f43`)

## Motivation and current defects

Four concrete defects, each addressed by exactly one design item:

1. Unserialized framebuffer access. `AppListActivity::selectIndex` /
   `moveSelection` (`firmware/src/activities/apps/AppListActivity.cpp:271-284`)
   mutate `selectedIndex_` and `updateRequired_` without holding
   `renderingMutex_`, while `displayTaskLoop` (`:160-171`) reads both under the
   mutex every 10 ms. A key arriving mid-render can tear a frame or collapse two
   moves into one visible step. MyLibrary and Home run their own task variants
   of the same pattern, so the hazard is systemic, not local to AppList.
2. First-paint phantom edges. Boot performs no blocking first paint and no
   held-key absorb (contrast CrossMux `src/main.cpp:778-787`). A key held
   across first paint is therefore delivered as a fresh press edge to the first
   activity, where it can activate the wrong row or open the wrong destination.
3. Scattered navigation mapping. `util/ButtonNavigator` resolves next/previous
   to raw `Down`/`Right` and `Up`/`Left` (`firmware/src/util/ButtonNavigator.h:47-52`),
   and several activities bypass the navigator with hand-rolled edge checks.
   There is one subsystem to unify into, not a second one to build.
4. Release double-fire. m4-stack has no consume-once release semantics
   (`MappedInputManager` has no `wasLongPressed`, no suppression state). Any
   press action that also arms on release can fire twice; long-press sites
   currently defend themselves ad hoc with level reads plus timestamp compares.

## Exclusions

The following are explicitly out of Phase 1 and must not enter through
follow-on edits to this spec. Each is deferred to a later phase or rejected:

- No synthetic-input FIFO and no InteractionBuffer of any kind. The one-shot
  synthetic slot stays as is.
- No changes to one-shot synthetic semantics, the 40 ms injection gate, or the
  `beginFrame` clear. The QEMU/m4adb input contract is frozen.
- No process-wide render-dispatch flag while per-activity render tasks remain.
  There is no global `requestedUpdate`, no central render task, no shared
  dispatcher in this phase.
- No wholesale ActivityManager port. No activity stack, no pending-action
  model, no `goTo…` fan-out, no MainTab or exclusive-storage handling.
- No font work of any kind (no cpfont reader, no rasterizer change, no
  manifest or download flow).
- No touch-classifier widening (no new gestures, thresholds, or frame
  handling; the wt-touch-p0 seam is untouched).
- No plugin changes (no `.m4x`, manifest, registry, Lua host, or provider
  binding changes).
- No per-activity static memory estimates and no navigation refusal. The
  governance ledger is not wired in this phase, and no transition may be
  refused for memory reasons.
- No SDK sync. The vendored `open-m4-sdk` trees are not touched.

## Architecture boundaries

Phase 1 adds a thin discipline layer beside existing structures; it does not
reorganize them. The layer consists of a render guard plus a boot-only blocking
primitive, a two-value button-enum extension with mapper support, a small
per-frame suppression state, and a boot-time settle step. Concretely out of
contact: `firmware/open-m4-sdk`, `firmware/lib`, the debug bridge
(`firmware/src/debug/M4SerialDebugBridge.cpp`), the synthetic slot and rate
gate (`MappedInputManager::injectSyntheticKey`, `kSynthMinIntervalMs`),
`M4MemoryGovernance`, `M4NavigationSupervisor`, fonts, touch classification,
and plugins.

Top-level lifecycle is unchanged: `currentActivity`,
`deferredDeleteActivity`, `enterNewActivity`, and `exitActivity` in
`firmware/src/main.cpp:412-430` keep their signatures and semantics, as does
`ActivityWithSubactivity::pumpSubActivityFrame`. Per-activity render tasks stay
in place; the design constrains how they touch shared state, not when they run.

## Existing components reused vs adapted from CrossMux

| CrossMux source | m4-stack disposition |
|---|---|
| `src/activities/RenderLock.h` — RAII over one mutex, `unlock`, `peek` | Port nearly verbatim as a new `M4RenderGuard` over one process-wide mutex owned alongside `currentActivity` in `main.cpp`. Only the name and owner change. |
| `ActivityManager::requestUpdateAndWait` (`ActivityManager.h:169`, never called from render task or under lock) | Adapt as a boot-only blocking primitive with identical misuse rules. The notify-a-task half is implemented directly (run the foreground render synchronously under guard) because there is no central render task yet. |
| Boot first-paint plus absorb (`src/main.cpp:778-787`: blocking paint, then two `gpio.update()` calls spaced past the 5 ms debounce) | Port nearly verbatim into the m4-stack boot path after the initial activity enter and before `waitForPowerRelease` (`firmware/src/main.cpp:1279` region). Spacing uses the existing 10 ms delay idiom. |
| `MappedInputManager::wasLongPressed` / `suppressNextRelease` / `consumeSuppressedRelease` (`MappedInputManager.cpp:488-511`) and frame-top consumption (`ActivityManager.cpp:115`) | Adapt minus BLE edge arrays and minus the 16-button mask (9-button scope). Frame-top check is placed in the m4-stack main loop between gesture handling and `currentActivity->loop()`. |
| `ButtonNavigator` press/release/continuous structure plus `NavNext`/`NavPrevious` logical buttons (`util/ButtonNavigator.h:49-50`, `MappedInputManager.h:27-28`) | Adapt: port the two logical enumerators and the navigator structure; resolve them to Down/Right and Up/Left under the Phase 1 portrait policy with no orientation swap. |
| `ActivityManager` stack, pending actions, render task, `replaceActivityWith`, `goTo…` methods | Not copied. Explicitly excluded above. |
| `UiSceneActionQueue` fixed-capacity ring | Not copied in Phase 1 (no buffer is built). Recorded here only as the template for a later phase, so a future buffer matches house style. |

## Item 1 — Render guard plus boot-only blocking primitive

A single process-wide rendering mutex is created alongside `currentActivity`
in `main.cpp` and wrapped in a new RAII guard type following `RenderLock`
(move-forbidden, explicit `unlock`, static `peek` for early-abort checks).
Every framebuffer mutation and every display-submission call for an activity
instance happens while holding that instance's guard acquisition. Pure state
writes that a render pass consumes (selection index, dirty flags, menu
indices) take the guard for the write; this closes the AppList defect where
`selectIndex` writes outside the mutex the display task reads under.

`requestUpdateAndWait` is introduced with CrossMux-identical misuse rules: it
must never be called from a render task or while holding the guard (assert in
debug builds, diagnostic plus immediate return in release). In Phase 1 it has
exactly one caller, the boot first-paint path: enter the initial activity, run
its render synchronously under guard on the main task, and block until display
submission returns. Because submission is a synchronous driver call, the wait
reduces to the guarded render itself; the primitive, its timeout, and its
assertions exist now so later task consolidation reuses the call sites rather
than inventing new ones. No per-frame caller is added and no global dispatch
flag is created, since per-activity tasks keep their own wake conditions.

## Item 2 — First-paint input settle

Immediately after the blocking first paint and before `waitForPowerRelease`,
the boot path performs two `gpio.update()` samples spaced 10 ms apart. Any
physical key held across the paint boundary thereby commits to level state
through the 5 ms debounce (`DEBOUNCE_DELAY` in
`firmware/open-m4-sdk/libs/hardware/InputManager/include/InputManager.h:298`)
without producing press or release edges, so the first dispatched frames
observe `isPressed` with silent edges. Synthetic one-shot events are
unaffected by construction (they bypass the debounced sampler), and no
journey timing changes: settle runs once at boot, costs 20 ms wall time, and
never runs on the input hot path.

## Item 3 — Unify navigation into the existing ButtonNavigator

`MappedInputManager::Button` gains `NavNext` and `NavPrevious`, appended after
`PageForward` so all nine existing enumerator ordinals are unchanged. The
existing `mapButton` resolves `NavNext` to Down-or-Right and `NavPrevious` to
Up-or-Left under the Phase 1 portrait policy; orientation-dependent axis
swapping is deferred (see Non-goals). `ButtonNavigator::getNextButtons` and
`getPreviousButtons` return the single logical button each; the
press/release/continuous helpers and all index/page math are untouched.

Adoption respects axis semantics so behavior cannot change silently. Linear
lists (MyLibrary file rows, settings rows) move to `onNextRelease` /
`onPreviousRelease` with the logical buttons. Grid surfaces keep explicit
axes: AppList vertical moves stay on the Down/Up edge sets via the existing
explicit-button overloads (`onRelease` over a button list), because AppList
assigns Left to the uninstall dialog and Right to install — folding those
into `NavPrevious`/`NavNext` would remap working keys. No second navigator
subsystem is created; every navigation call site ends on `ButtonNavigator`.

## Item 4 — Suppressed-release / consume-once semantics

`MappedInputManager` gains per-button long-press state: a one-shot threshold
event while held, a suppression record for the consumed press, and a
frame-top `consumeSuppressedRelease` check placed in the main loop between
gesture handling and `currentActivity->loop()`. When the check reports a
consumed release, the frame dispatches no activity input (render tasks run
independently). A fired long-press therefore produces exactly one action and
its release edge never dispatches a second one; edges on unrelated buttons in
the same frame still dispatch normally. Thresholds at existing long-press
sites (long-press Back to home/root with their current millisecond constants)
are preserved bit-for-bit; only the firing and release-consumption mechanism
changes, and only where new contracts cover it.

## Render ownership rule

Exactly one execution context emits frames per activity instance. For an
activity with a display task, that task is the sole submitter; for an
activity with a synchronous render path, the main-task render call is the
sole submitter. All framebuffer mutation and all display submission occur
under a guard acquisition held by the submitting context. Child activities
render under the same rule: `ActivityWithSubactivity` display paths skip or
defer exactly as today (`displayTaskLoop` already skips when a sub-activity
is present), with guard acquisitions added around the existing submit calls
rather than new submit calls. Nothing in Phase 1 adds, removes, or reorders
a submit call; it only serializes them.

## Error and timeout behavior

`requestUpdateAndWait` takes a bounded wait of 2 seconds, chosen to exceed
the slowest FAST submit path with margin while keeping a wedged boot
diagnosable rather than silent. On timeout it emits the existing activity
enter/exit style serial diagnostic, records a boot-render-miss flag readable
through the established debug/status surface, and proceeds to settle anyway;
boot never halts on a missed first paint. Misuse (invocation from a render
task or under a held guard) asserts in debug builds and degrades to a
diagnostic plus immediate return in release builds. Guard acquisition on new
paths uses bounded waits with a missed-render counter; no new unbounded
`portMAX_DELAY` wait is introduced. Governance refusal does not exist in this
phase, so no transition can fail for memory reasons.

## Data and control flow

Steady-state frame (insertions marked): `gpio.update()` → `beginFrame()` →
bridge poll → global gestures → [new] suppressed-release check (consumed
frame skips to end-of-loop bookkeeping) → `currentActivity->loop()` with
navigator calls resolving through logical buttons where adopted → activity
signals its own task wake (unchanged mechanism) → task renders and submits
under guard → `delay(10)` / `yield()`. Boot sequence: enter initial activity
→ [new] blocking guarded first paint → [new] two-sample settle → existing
`waitForPowerRelease` → loop. Synthetic injection, the 40 ms gate, and the
`beginFrame` clear keep their exact positions and semantics.

## Invariants and acceptance tests

Each invariant ships with host contract tests written first (RED) alongside
the existing suites in `firmware/tests/native_app/` and `tests/contracts/`;
the behavior flips only when the tests pass (GREEN). Existing QEMU/m4adb
suites run unchanged as regression gates throughout.

INV-R1 — Guarded emission. For every activity instance, all framebuffer
mutation and all display submission occur under a guard acquisition held by
the single submitting context for that instance.
Acceptance: (1) host contract with a recording renderer and an instrumented
guard — N signaling threads plus guarded state writes produce a monotonic
submission generation with no interleaved or torn submissions; (2) AppList
contract — a selection write concurrent with a render pass leaves index, flag,
and painted highlight mutually consistent; (3) full QEMU journey suite green
with no new serial render anomalies.

INV-I1 — Silent settle. After the blocking first paint completes, a physical
key held across the paint boundary is observable as level (`isPressed`) with
no press or release edge on the first dispatched frames.
Acceptance: (1) host sampler test — a level held across two updates spaced
past the debounce commits to `currentState` with `pressedEvents` and
`releasedEvents` both clear; (2) boot-order assertion — setup performs paint,
then settle, then power-release wait, verified against the serial boot log on
a QEMU boot; (3) synthetic events remain one-shot through settle (existing
synthetic journey assertions unchanged and green).

INV-N1 — Logical navigation equivalence. Under the Phase 1 portrait policy,
`NavNext` fires exactly when Down-or-Right would have fired and `NavPrevious`
exactly when Up-or-Left would have fired; all existing index and page math is
unchanged.
Acceptance: (1) host contract enumerating every button-by-edge combination
through both the legacy getters and the logical mapping, asserting identical
outcomes; (2) AppList grid contract — vertical moves still step one row and
Left/Right keep their uninstall/install bindings after adoption; (3) MyLibrary
journey navigation (down/confirm/back) green without timing adjustments.

INV-S1 — Consume-once release. A fired long-press produces exactly one action
and its release edge never dispatches again; edges on unrelated buttons in
the same frame still dispatch; the next fresh press on the same button works
normally.
Acceptance: (1) host contract driving press → hold past threshold → release,
asserting one callback, a consumed release, and a subsequent fresh press
firing again; (2) mixed-frame contract asserting an unrelated button edge in
the release frame still dispatches; (3) long-press Back journeys (home/root)
green with thresholds unchanged.

## Migration order

AppList first: it carries the highest journey traffic and the confirmed
unlocked-write race, so guard adoption plus navigator-explicit overloads land
there with INV-R1 and INV-N1 contracts. MyLibrary second: linear rows take
the logical `onNextRelease` / `onPreviousRelease` cutover under INV-N1, plus
guard adoption under INV-R1. Home third: same pattern, with attention to its
backend scene tasks sharing framebuffer state. Remaining activities adopt
only as needed; each adoption is a self-contained diff behind that
activity's contracts, and unmigrated activities behave exactly as today
because every new mechanism defaults off.

## TDD and contract strategy

Follow the established P0–P2 rhythm: RED host contracts first, implementation
second, QEMU/m4adb suites as unchanged regression gates. No behavior flips
without a covering contract from the invariant list above. Serial-log
assertions may gain new lines (settle marker, boot-render-miss diagnostic)
but no existing line may change format; any journey timing stays exactly as
pinned. Memory-governance contracts are untouched — attribution wiring is a
later phase, so nothing here may alter allocation behavior or heap baselines.

## Rollback boundaries

Each item reverts independently without touching the others. Render guard:
remove guard acquisitions and restore the local flags; the shared mutex
deletes with no callers. Blocking primitive: delete the single boot call
site; boot returns to enter-then-wait-for-release. Settle: delete the
two-sample step; boot edges behave as today. Navigator: revert the two
getters to raw button lists; the appended enumerators remain inert.
Suppression: default the frame-top check off in one place; long-press sites
keep their current level-read behavior. No item removes a legacy path in the
same diff that introduces its replacement; removals, if ever, follow in a
later phase after the replacement has ridden the journeys.

## Non-goals

Orientation-axis generalization for logical buttons; any global render
dispatcher, shared dispatch flag, or task consolidation; synthetic-path
changes of any kind; FIFO or buffer construction; governance wiring,
estimates, or refusal; font, touch-classifier, plugin, BLE, or SDK-sync work;
new activities, screens, or user-visible behavior of any kind. Phase 1 is
observably behavior-identical except where an invariant acceptance test
pins the intended fix.

## Implementation sequencing

Sequencing is ordered by dependency and blast radius, each step carrying its
invariant contracts: first the render guard with AppList adoption (INV-R1);
second the boot blocking paint plus settle (INV-I1, boot-only, independent of
activities); third the logical-button extension with MyLibrary linear-list
adoption (INV-N1); fourth suppression state with the frame-top check and one
long-press site conversion (INV-S1); then Home adoption of guard plus
navigator to close the top-three-activity set. Later phases (global dispatch,
buffering, governance attribution) build on these call sites and are not
started here.
