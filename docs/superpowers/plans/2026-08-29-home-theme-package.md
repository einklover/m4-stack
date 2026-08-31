# M4 Home Theme Package — TDD Implementation Plan

> Implement the approved design in `docs/superpowers/specs/2026-08-29-home-theme-package.md`. Preserve the existing dirty Home worktree; do not reset unrelated changes.

## Baseline / safety

- [ ] Record current branch/HEAD and `git status --short`; do not clean or reset the worktree.
- [ ] Run existing focused Home native tests and `simulator/tests/track_a_geometry.cpp` to establish the baseline.
- [ ] Keep the existing hard-coded Home renderer available until the package path passes all validation.

## Task 1 — Host theme compiler and binary format fixture

Files:
- Create `firmware/tools/compile_home_theme.py`
- Create `firmware/tests/test_home_theme_compiler.py`
- Create `themes/mofei-classic/theme.json`
- Add the supplied reference JPEG under `themes/mofei-classic/assets/` as the first authoring source; generated runtime assets are committed.

TDD:
- [ ] RED: tests for schema rejection, 480×800 conversion, explicit erase regions, deterministic bytes, magic/version/section table, CRC.
- [ ] Implement only enough compiler to make tests GREEN.
- [ ] Add finite maps for slot/binding/action/font enums; unknown values are compile errors.
- [ ] Emit one `.m4theme` file plus optional generated PROGMEM header from identical bytes.
- [ ] Compile the supplied 919×1536 JPEG into the first `mofei-classic` pack.
- [ ] Verify generated background is 48,000-byte 1bpp raster and configured dynamic regions are white.

## Task 2 — Runtime format types, parser and validation

Files:
- Create `firmware/src/components/home_theme/HomeThemeFormat.h`
- Create `firmware/src/components/home_theme/HomeThemePackage.h/.cpp`
- Create `firmware/tests/native_app/test_home_theme_package.cpp`
- Add a small compiler-generated binary fixture under test data if needed.

TDD:
- [ ] RED: valid pack parse; bad magic; bad version; wrong resolution; bad total size; bad CRC; truncated section table; overflow/overlap; too many slots/assets; invalid rect; invalid binding/target; duplicate focus order.
- [ ] Implement fixed little-endian decoding without reinterpret-casting untrusted packed structs.
- [ ] Enforce v1 limits from the spec before allocation/read.
- [ ] GREEN: host-native parser tests.

## Task 3 — Built-in and SD range sources + selection fallback

Files:
- Create `firmware/src/components/home_theme/HomeThemeSource.h`
- Create `firmware/src/components/home_theme/BuiltinHomeThemeSource.h/.cpp`
- Create `firmware/src/components/home_theme/SdHomeThemeSource.h/.cpp`
- Create `firmware/src/components/home_theme/HomeThemeRepository.h/.cpp`
- Create/update generated `firmware/src/generated/mofei_classic_m4theme.h`
- Create `firmware/tests/native_app/test_home_theme_source.cpp`

TDD:
- [ ] RED: memory source readAt bounds and exact bytes.
- [ ] RED: repository prefers valid `/.crosspoint/themes/active.m4theme`, otherwise selects built-in.
- [ ] RED: corrupt/truncated external pack falls back to built-in and records diagnostic status.
- [ ] Implement sources with bounded range reads; never allocate `total_size`.
- [ ] Make built-in and SD providers feed the same `HomeThemePackage` parser.

## Task 4 — Home theme geometry / typed targets / focus traversal

Files:
- Create `firmware/src/components/home_theme/HomeThemeTarget.h`
- Create `firmware/src/components/home_theme/HomeThemeGeometry.h/.cpp`
- Create `firmware/tests/native_app/test_home_theme_geometry.cpp`

TDD:
- [ ] RED: hit testing returns `RecentBook(0..3)` / typed actions from slot records.
- [ ] RED: focus traversal follows `focus_order`, not slot-array order.
- [ ] RED: non-focusable visual slots never become navigation targets.
- [ ] Implement `hitTest(x,y)`, `firstFocus()`, `nextFocus()`, `previousFocus()`, `rectForTarget()` against parsed records.
- [ ] Verify the first theme reproduces current HomeRef target rects without reading HomeRef in the new geometry code.

## Task 5 — View model and dynamic binding resolver

Files:
- Create `firmware/src/components/home_theme/HomeThemeViewModel.h/.cpp`
- Create `firmware/tests/native_app/test_home_theme_bindings.cpp`

TDD:
- [ ] RED: recent title/author/source/progress values resolve for indexes 0..3.
- [ ] RED: missing recent entries resolve to empty/not-present state safely.
- [ ] RED: Wi-Fi binding resolves current state without renderer owning network logic.
- [ ] Implement finite enum switch; no string expression evaluator on-device.

## Task 6 — Renderer: background, text/progress, cover masks, overlay/focus

Files:
- Create `firmware/src/components/home_theme/HomeThemeRenderer.h/.cpp`
- If profiling requires it, add one narrow packed-row/run helper to `firmware/lib/GfxRenderer/GfxRenderer.h/.cpp`; otherwise keep row run decoding inside renderer.
- Create `firmware/tests/native_app/test_home_theme_renderer_geometry.cpp` and/or pure helper tests.

TDD:
- [ ] RED: 1bpp row decoding produces correct black runs and respects stride.
- [ ] RED: cover corner inset/radius helper identifies pixels/spans to erase on all four corners.
- [ ] RED: progress clamps 0..100 and uses theme rect/radius.
- [ ] RED: focus draws using target rect/inset after overlay.
- [ ] Implement four ordered phases: background -> dynamic slots -> overlay/masks -> focus.
- [ ] Background reads one 60-byte row at a time from `IHomeThemeSource`.
- [ ] Reuse existing Bitmap/cover-cache pipeline for covers.
- [ ] Apply procedural white corner erase then rounded border; do not use alpha PNG.

## Task 7 — Integrate package renderer into HomeActivity without deleting legacy path

Files:
- Modify `firmware/src/activities/home/HomeActivity.h/.cpp`
- Minimal modification to `firmware/src/components/UITheme.h/.cpp` only if necessary to expose/own the Home theme repository; do not convert non-Home pages.
- Update focused Home tests.

TDD:
- [ ] RED: package renderer is selected for Home when built-in package validates.
- [ ] RED: package hit target dispatches to existing callbacks (recent/files/weread/fanqie/jinjiang/history/apps/settings).
- [ ] RED: keyboard/swipe focus uses typed focus order.
- [ ] RED: forced package failure reaches safe legacy/built-in fallback instead of blank Home.
- [ ] Integrate with existing recent-cover loading/buffer lifecycle conservatively.
- [ ] Keep non-Home Fengyan/Lyra behavior unchanged.

## Task 8 — First theme resource and simulator fixture

Files:
- Finalize `themes/mofei-classic/theme.json`
- Generate/commit `firmware/src/generated/mofei_classic_m4theme.h` and optionally the canonical `.m4theme` artifact in an appropriate resource/test location.
- Update `simulator/tools/seed_home_recents.py` or add `simulator/tools/install_home_theme.py` for external theme testing.

Validation:
- [ ] The reference JPEG source dimensions are asserted as 919×1536.
- [ ] Generated first theme uses the measured 480×800 Home geometry.
- [ ] Seed four recent books with nonzero hero progress and real/fixture BMP covers so all cover slots can be inspected.
- [ ] Test external `active.m4theme` injection into simulator SD.
- [ ] Test a deliberately corrupt external pack and verify built-in fallback visually/logically.

## Task 9 — Verification and cleanup

Run from fresh commands and record exact results:

- [ ] `python3 -m pytest firmware/tests/test_home_theme_compiler.py`
- [ ] all new native Home-theme tests
- [ ] existing `test_home_reference_geometry`, `test_home_reference_theme`, `test_home_critical_ui`, and `track_a_geometry` (or documented migrated equivalents)
- [ ] `pio run -e murphy_m4`
- [ ] `pio run -e murphy_m4_qemu_plugin` (or repository canonical QEMU plugin build command)
- [ ] launch real simulator/QEMU and capture framebuffer with existing `m4sim screenshot` (not image generation)
- [ ] compare structural coordinates and verify no stale sample JPEG text/covers show through dynamic regions
- [ ] inspect heap/PSRAM/logs for pack load; confirm no whole-pack allocation
- [ ] `git diff --check`
- [ ] final code review: theme geometry is not duplicated in HomeActivity/TouchHitGeometry for the new path; external corruption cannot bypass fallback.

## Task 10 — Deferred cleanup (only after verified)

- [ ] Do **not** delete legacy `HomeRef`/Fengyan Home renderer in this implementation unless both simulator and target-device verification justify it.
- [ ] Mark redundant hard-coded Home geometry as legacy and schedule later removal.
- [ ] Future theme-selection UI/multiple installed theme catalog is out of v1; v1 uses `/.crosspoint/themes/active.m4theme` plus embedded default.
