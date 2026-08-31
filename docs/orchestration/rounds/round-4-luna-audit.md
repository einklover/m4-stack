# Round 4 — Luna audit: drawer handoff and three-column layout

Worktree: `/Volumes/z/paseo/workspaces/paseo/worktrees/041rfr5o/m4-home-luna-audit`  
Branch: `agent/home-luna-audit`  
Base: `96a64a0`

## 花屏: root cause / fix commit / unverified

- **Root cause:** `AppListActivity::displayTaskLoop()` tested
  `updateRequired_ && !subActivity` before taking `renderingMutex_`. A valid
  interleaving was: the display task passed that test; the main activity loop
  entered `openSelected()`; `openSelected()` took the same mutex and installed
  `NativeAppActivity` as the child; then the display task acquired the mutex and
  still called the old drawer `render()`. The Native child renders through the
  same `GfxRenderer` framebuffer/display path, so the stale drawer clear/draw/
  `displayBuffer()` could overlap the Native first frame and produce a mixed
  e-ink frame.
- **Fix commit:** `a206289` — `round-4(luna): fix drawer handoff and use three columns`.
  The display task now takes the existing mutex before rechecking both
  `updateRequired_` and `subActivity`; it clears the update flag only inside
  that confirmed no-child branch. `openSelected()` continues to install the
  native or Lua child while holding the same mutex.
- **Unverified:** The physical device/QEMU visual symptom is not directly
  reproduced in this lane because PIO, QEMU, `./m4sim`, and hardware access
  were explicitly forbidden. The host handoff contract is green and proves the
  stale pre-lock ordering is gone; a fresh first-frame device/QEMU capture is
  still required by the coordinator.

## Audit evidence

The JJWXC package is `com.jjwxc.client`, with `runtime: native` and
`entry: main.xml`. Its XML starts a compact text/list screen and does not use an
image or oversized geometry node. The drawer path is:

`AppListActivity::openSelected()` → `ActivityWithSubactivity::enterNewActivity()`
→ `NativeAppActivity::onEnter()` → Native `loop()`/`render()`.

`NativeAppActivity::onEnter()` loads the document and marks its first render
required; its `render()` clears and submits the shared renderer buffer from the
main activity loop. The reviewed Native pixel paths perform screen bounds
checks, and no independent Native geometry/font/UAF smoking gun was found.
The existing refresh policy also normalizes ordinary UI/plugin submissions to
the fast UI path; no refresh-mode change was made.

## Three-column status

Implemented in `AppListActivity.cpp`:

- `kDrawerColumns = 3` with the existing shared draw/hit-test and keyboard
  movement calculations.
- Tile height increased from 100 to 120 and icon slot from 64 to 80.
- Native 62×64 plugin icons are vertically centered in the slot.
- Drawer labels use `UI_12_FONT_ID` and retain
  `utf8EllipsizeChars(item.label.c_str(), kDrawerLabelMaxChars)` with the
  existing four-codepoint limit.
- Built-in icon bitmaps remain their safe native 32×32 size; no unsafe bitmap
  upscaling was introduced.

## Host commands and results

All commands ran from the worktree above.

- `python3 firmware/tests/test_m4_dependency_bootstrap_contract.py` — PASS.
- RED check before the patch:
  `/opt/homebrew/bin/g++-14 -std=c++17 firmware/tests/native_app/test_app_drawer_handoff.cpp -o /tmp/test_app_drawer_handoff`, then `/tmp/test_app_drawer_handoff` — expected assertion failure on the old pre-lock child gate.
- RED check before the patch:
  `/opt/anaconda3/bin/pytest -q firmware/tests/test_app_drawer_3col.py` — expected 2 failures for four columns and 100px tiles.
- `/opt/homebrew/bin/g++-14 -std=c++17 firmware/tests/native_app/test_app_drawer_handoff.cpp -o /tmp/test_app_drawer_handoff`, then `/tmp/test_app_drawer_handoff` — PASS.
- `/opt/anaconda3/bin/pytest -q firmware/tests/test_app_drawer_3col.py` — PASS, 2 tests.
- `/opt/homebrew/bin/g++-14 -std=c++17 firmware/tests/native_app/test_app_drawer.cpp -o /tmp/test_app_drawer`, then `/tmp/test_app_drawer` — PASS.
- `/opt/homebrew/bin/g++-14 -std=c++17 -I firmware/lib/Utf8 firmware/lib/Utf8/Utf8.cpp firmware/tests/native_app/test_utf8_ellipsize.cpp -o /tmp/test_utf8_ellipsize`, then `/tmp/test_utf8_ellipsize` — PASS.
- `/opt/homebrew/bin/g++-14 -std=c++17 firmware/tests/native_app/test_gfx_refresh_policy.cpp -o /tmp/test_gfx_refresh_policy`, then `/tmp/test_gfx_refresh_policy` — PASS.
- `/opt/anaconda3/bin/pytest -q firmware/tests/test_home_app_drawer.py` — PASS, 6 tests.
- `git diff --check` and `git diff --cached --check` — PASS.

PIO, QEMU, `./m4sim`, hardware flash, push, reset, and clean operations were
not run.

## Files touched by this lane

- `firmware/src/activities/apps/AppListActivity.cpp`
- `firmware/tests/native_app/test_app_drawer_handoff.cpp` (new)
- `firmware/tests/test_app_drawer_3col.py` (new)
- `docs/orchestration/rounds/round-4-luna-audit.md` (this report)

The pre-existing dirty `HomeSceneAssetDecoder.cpp`,
`firmware/tests/native_app/test_home_lifecycle_uaf.cpp`, and untracked
orchestration assets were left untouched and unstaged.
