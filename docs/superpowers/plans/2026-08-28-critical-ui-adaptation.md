# M4 Critical UI Adaptation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or equivalent TDD/review discipline. Parallel tracks use isolated Paseo worktrees and may not edit files owned by another track.

**Goal:** Bring the contributor preview's strongest visual hierarchy into the real M4 firmware as a restrained 480×800 e-ink UI grammar without changing proven navigation, reader, network, plugin, font, or flash behavior.

**Architecture:** Keep `feature/critical-ui-adaptation` as the integration branch. Three child branches start from the same docs baseline and work in parallel with strict file ownership: Track A defines shared visual grammar/contracts, Track B adapts Home only, Track C adapts provider/native-app screens only. The coordinator integrates reviewed commits, reconciles the frozen interface, runs production build + simulator + screenshot review, then an independent reviewer audits the combined branch.

**Tech Stack:** ESP32-S3 C++/Arduino/PlatformIO, native host C++ tests, Python preview tools, Paseo/QEMU `m4sim`.

**Spec:** `docs/superpowers/specs/2026-08-28-critical-ui-adaptation-design.md`

## Global Constraints

- Critical adaptation, not imitation: functional clarity and proven interaction behavior beat visual fidelity.
- Production geometry is 480×800; never copy 400×800/browser constants mechanically.
- No external screenshots, bundled fonts, unverified SVG/icon caches, HTML/CSS/CDN assumptions, generated headers, or assets with unclear provenance enter firmware.
- Preserve Chinese readability through existing runtime UI font roles and UTF-8-safe truncation/wrapping.
- Preserve touch hit targets and existing back/home/footer/navigation semantics.
- Avoid full-page dot grids, dense decorative textures, heavy repeated borders, and unnecessary large black fills because of e-ink ghosting/refresh cost.
- No intentional changes to reader pagination/lifecycle, Wi-Fi, provider/network business logic, WeRead, plugin runtime/packaging, font engine, partitions, boot flow, or flashing.
- Major changes require focused tests, fresh production build, mandatory simulator smoke, and key-screen visual review. `SMOKE PASS` alone is not visual acceptance.
- No real-device flash until the integrated branch is green in simulator and visual review.

## Frozen Parallel Interface Contract

Tracks B and C may rely on this contract conceptually but MUST NOT edit Track A files. Track A owns the concrete implementation.

`M4CriticalUiMetrics` (exact final name may be implemented in the existing style/theme namespace rather than a new global type) provides only small geometry/style values derived from current `UITheme::getMetrics()`:

- `sectionGapPx`: vertical separation between major content sections.
- `cardInsetPx`: internal card/list padding.
- `dividerInsetPx`: horizontal inset for sparse section rules.
- `selectedBorderPx`: selected-state outline thickness.
- `selectedFill`: boolean/enum policy allowing a bounded fill only for selected/primary state.

Rules:
- Values must preserve 480×800 bounds and existing touch rectangles.
- No new bitmap/font/icon dependency.
- Track B/C should wrap any needed local constants behind screen-local helpers and report any missing shared metric to integration rather than modifying Track A-owned files.

## Branch / Workspace Topology

Integration branch: `feature/critical-ui-adaptation`

Child branches, all branching from the plan commit on the integration branch:

- `feature/critical-ui-grammar`
- `feature/critical-ui-home`
- `feature/critical-ui-provider`

Use separate Paseo-managed worktree workspaces. Do not share build/output directories across active agents when avoidable.

## File Ownership

| Track | Owns production files | Owns tests | Must not edit |
| --- | --- | --- | --- |
| A Grammar | `firmware/src/components/UITheme.*`, `firmware/src/components/themes/fengyan/FengyanTheme.*`, narrowly necessary shared style helpers | new focused grammar/style contract test(s) | `HomeActivity.*`, `M4NativeUi.*`, `NativeAppActivity.*`, `NativeProviderBookActivity.*` |
| B Home | `firmware/src/activities/home/HomeActivity.*` | Home-specific new tests plus existing `test_home_book_detail_*` invocation | Track A files; provider/native-app files |
| C Provider | `firmware/src/apps/native/M4NativeUi.*`, `firmware/src/activities/apps/NativeAppActivity.*`, `firmware/src/activities/apps/NativeProviderBookActivity.*` | provider/native-UI-specific tests | Track A files; Home files |
| Integration | may resolve all after child commits are reviewed | whole-suite / simulator / visual evidence | no feature expansion |

## Task 1 — Track A: Shared Pixel-Clarity Grammar

**Files:**
- Modify: `firmware/src/components/UITheme.h`
- Modify: `firmware/src/components/UITheme.cpp` only if dispatch/metrics wiring requires it
- Modify: `firmware/src/components/themes/fengyan/FengyanTheme.h`
- Modify: `firmware/src/components/themes/fengyan/FengyanTheme.cpp`
- Create: `firmware/tests/native_app/test_critical_ui_grammar.cpp`

**Produces:** a minimal reusable geometry/style contract for sparse dividers, consistent insets, and bounded selected-state treatment. No screen-specific navigation or content logic.

- [ ] Write RED tests asserting metrics stay inside 480×800, non-selected states do not request large filled regions, and selected borders/insets are nonzero but bounded.
- [ ] Compile/run the focused native test using the repository's established native-app test pattern; capture genuine failure before implementation.
- [ ] Implement the smallest theme/style changes needed. Reuse existing renderer primitives and current font/icon facilities only.
- [ ] Run the focused test GREEN and run `firmware/tests/native_app/test_runtime_ui_font_policy.cpp` / relevant theme-compatible checks.
- [ ] Run `git diff --check`; confirm only Track A-owned files changed.
- [ ] Commit: `feat(m4): add critical UI visual grammar`.

## Task 2 — Track B: Home Adaptation

**Files:**
- Modify: `firmware/src/activities/home/HomeActivity.h` only if a screen-local helper/state declaration is required
- Modify: `firmware/src/activities/home/HomeActivity.cpp`
- Create: `firmware/tests/native_app/test_home_critical_ui.cpp`
- Run existing: `firmware/tests/native_app/test_home_book_detail_contract.sh`, `firmware/tests/native_app/test_home_book_detail_meta.cpp`, `firmware/tests/native_app/test_touch_regressions.cpp`

**Consumes:** frozen visual grammar semantics; must not edit shared theme files. Existing touch rectangles and navigation actions remain authoritative.

**Produces:** improved recent-book hierarchy, restrained card/section spacing, and clearer selected menu treatment without altering Home actions.

- [ ] RED: test Home layout helper/contract for 480×800 bounds, recent-book metadata order, and visible geometry contained inside unchanged touch targets.
- [ ] Run focused RED test and existing Home/touch regression anchors.
- [ ] Implement screen-local rendering changes only. Preserve cover loading/cache path, book action mapping, and current touch/navigation policy.
- [ ] Run focused GREEN plus existing Home/detail/touch tests.
- [ ] Generate a Home preview/simulator frame if available in this worktree; record screenshot path for integration review.
- [ ] `git diff --check` and ownership check.
- [ ] Commit: `feat(m4): adapt Home pixel clarity UI`.

## Task 3 — Track C: Provider / Native UI Adaptation

**Files:**
- Modify: `firmware/src/apps/native/M4NativeUi.h` only if a screen-local/native node render helper is needed
- Modify: `firmware/src/apps/native/M4NativeUi.cpp`
- Modify: `firmware/src/activities/apps/NativeAppActivity.cpp`
- Modify: `firmware/src/activities/apps/NativeProviderBookActivity.cpp`
- Create: `firmware/tests/native_app/test_provider_critical_ui.cpp`
- Run existing: `firmware/tests/native_app/test_native_ui.cpp`, `firmware/tests/native_app/test_native_provider_detail_touch.cpp`, provider metadata/history tests as applicable

**Consumes:** frozen visual grammar semantics; must not edit Track A theme files.

**Produces:** clearer provider category/list presentation and detail hierarchy: title → author/source → chapter/reading state → primary action → update info → description.

- [ ] RED: assert provider detail ordering/geometry and that visual rectangles remain within existing detail touch policy regions.
- [ ] Run RED plus `test_native_ui.cpp` and `test_native_provider_detail_touch.cpp`.
- [ ] Implement visual-only changes. Do not alter HTTP/provider/cache/login/business logic or action routing.
- [ ] Run focused GREEN plus native UI/detail touch and metadata/history regressions.
- [ ] Generate provider list/detail preview/simulator frames where available; record paths.
- [ ] `git diff --check` and ownership check.
- [ ] Commit: `feat(m4): adapt provider pixel clarity UI`.

## Task 4 — Integration on `feature/critical-ui-adaptation`

**Integration order:** Track A → Track B → Track C. Cherry-pick/rebase reviewed child commits; do not merge unrelated histories or external `a0bb304`.

- [ ] Verify integration branch contains only docs baseline before child commits.
- [ ] Apply Track A commit and run its focused tests.
- [ ] Apply Track B commit; resolve only interface mismatches against Track A, preserving Home-owned behavior.
- [ ] Apply Track C commit; resolve only interface mismatches against Track A, preserving provider-owned behavior.
- [ ] Review combined diff for duplicated constants/helpers. Consolidate only where the spec clearly requires shared behavior.
- [ ] Run existing critical regressions: Home/detail, native UI, provider-detail touch, touch regressions, runtime UI font policy, reader lifecycle/handoff, Wi-Fi, WeRead shard/shelf policies.
- [ ] Fresh production build: `cd firmware && /Users/zhouxinlai/.platformio/penv/bin/pio run -e murphy_m4 -j1`.
- [ ] Build simulator profile if needed, then run: `./m4sim test smoke --plugin-debug --skip-build --ready-seconds 90` and require `SMOKE PASS`.
- [ ] Capture actual simulator 480×800 Home, provider list/category, and provider detail frames. Compare against current baseline and external reference for hierarchy only—not pixel matching. Reject regressions in CJK readability, touch affordance, density, or ghosting-prone fill area.
- [ ] Record artifact size / flash delta; investigate material growth before proceeding.
- [ ] Commit integration-only fixes separately if required: `fix(m4): integrate critical UI adaptation`.

## Task 5 — Independent Whole-Branch Review

Reviewer reads spec + plan + full diff from plan baseline to integrated HEAD.

Review explicitly for:
- accidental copied external assets/constants
- touch/navigation semantic drift
- extra repaint/full-refresh behavior or ghosting-prone decoration
- CJK truncation/readability regressions
- provider/reader/Wi-Fi/WeRead/business-logic changes outside scope
- duplicate style logic that should live in Track A
- flash/RAM impact and maintainability
- simulator screenshot visual regressions

Any load-bearing finding returns to the owning track or integration for a focused fix and re-review. After fixes, rerun affected tests and the simulator smoke if production UI code changed.

## Acceptance Criteria

- Home, provider list/category, and provider detail visibly share a coherent restrained monochrome hierarchy.
- No full-page decorative texture and no large new nonfunctional filled areas.
- All visible layout is 480×800-safe and Chinese text remains readable.
- Existing touch/navigation behavior is unchanged in tests and simulator interaction.
- No external unverified asset/font/icon copied into production.
- Production build passes.
- Simulator returns `SMOKE PASS` on the final integrated tree.
- Key-screen simulator frames are manually reviewed and accepted; smoke alone is insufficient.
- Independent final review has no unresolved load-bearing findings.
- Only then may APP1-only real-device testing be considered.

## Rollback Points

Each track is a separate focused commit/branch. Rollback can remove Home or Provider adaptation independently while retaining the shared grammar. If shared grammar itself proves harmful, revert Track A and any integration shims; no data format, partition, or business-logic migration is introduced by this feature.
