# Round 4 — P0 drawer→plugin 花屏 + Home type/placeholder + AppList 3-col

Date: 2026-08-31  
Base tip: `96a64a0` (Wi-Fi mkdir fix already on all lanes)

## Severity order (human superseding request)

1. **P0 — Screen garble (花屏)** when opening a plugin from the app drawer, e.g. 晋江 `com.jjwxc.client`. Fix this first.
2. Home typography rebalance after TTF / `runtimeFontId` collapse.
3. Missing-cover placeholder (not empty white).
4. App drawer **3 columns** + larger icons/labels.
5. Prove in **QEMU first**; coordinator merges. No device flash this round unless human re-authorizes.

## Device / path facts (pre-fix)

- Flash tip on device: `96a64a0` APP1-only. Home + drawer screenshots under coordinator `tmp-home-screenshots/device-smoke-96a64a0/`.
- 晋江 manifest: `"runtime": "native"`, `"entry": "main.xml"`, id `com.jjwxc.client`.
- Drawer launch path: `AppListActivity::openSelected` → `NativeAppActivity` (native) or `AppRuntimeActivity` (Lua).
- Suspicious race (not proven — Luna must confirm or refute):
  - `AppListActivity` owns `displayTask` + `renderingMutex_`.
  - `openSelected` takes `renderingMutex_`, then `enterNewActivity(new NativeAppActivity(...))`, then gives mutex.
  - `displayTaskLoop` paints when `updateRequired_ && !subActivity`.
  - Holding the list mutex across activity enter / first paint, or racing e-ink `displayBuffer` / `clearScreen` with the parent display task, is a leading 花屏 hypothesis.
- Secondary hypotheses: FAST vs FULL refresh on handoff; stale framebuffer; Native UI Image/Tiles geometry overflow; font face not ready after recent TTF wiring; partial refresh tearing.

## Ownership (non-overlapping)

| Lane | Agent | Owns | Does not own |
|---|---|---|---|
| **Luna Max (P0 + drawer 3-col)** | Luna | Root-cause + minimal patch for drawer→plugin 花屏; AppList 3-col + icon/label sizing + host contracts | Home `theme.json`; QEMU session; expanding dirty `HomeSceneAssetDecoder.*` |
| **Muse Ultra (P0 assist + Home polish)** | Muse | If Luna pins a paint/buffer/font handoff bug in Native render path, apply the **minimal** paint fix; Home fonts + cover placeholder; QEMU proof of Home + drawer→晋江 first frame | Rewriting Scene framework; AppList grid columns (Luna); Settings hub/L2 |

## Luna whitelist

Allowed to edit/commit **only**:

- `firmware/src/activities/apps/AppListActivity.cpp`
- `firmware/src/activities/apps/AppListActivity.h`
- `firmware/src/activities/apps/NativeAppActivity.cpp` / `.h` (**only** if audit proves enter/exit/render handoff bug; keep diff surgical)
- `firmware/src/main.cpp` (**only** if launch routing from drawer is wrong; prefer not)
- **new** host/python tests under `firmware/tests/` / `firmware/tests/native_app/` for (a) launch handoff / mutex / no-paint-over-subactivity contracts and (b) drawer 3-col + label/icon size
- `docs/orchestration/rounds/round-4-luna-audit.md` (required report)

Forbidden:

- Home `theme.json`, `GfxSceneRenderer`, expanding dirty `HomeSceneAssetDecoder.cpp` / `test_home_lifecycle_uaf.cpp`
- Scene framework rewrite, Fengyan Home tab, Settings themes
- PIO / QEMU / `./m4sim` (host tests only this lane)
- `git push origin`, hardware flash, `git reset --hard` / `git clean`, `pkill`
- Do not invent a second app launcher

## Muse whitelist

Allowed to edit/commit **only**:

- `themes/murphy-default/theme.json`
- `firmware/tools/compile_home_theme.py` (only if FONT_MAP / compile needed)
- generated murphy-default header(s) under `firmware/src/generated/`
- `firmware/src/ui/scene/GfxSceneRenderer.cpp` / `.h` (missing-cover / placeholder paint only)
- `firmware/src/activities/home/HomeSceneAssetDecoder.cpp` / `.h` (compiled placeholder / fallback bind only)
- `firmware/src/activities/apps/NativeAppActivity.cpp` / `.h` **only** if Luna’s report assigns a paint/clear/displayBuffer fix and Luna is not already patching it (coordinate via report; do not both rewrite the same function blindly)
- `firmware/tests/test_home_typography_polish.py`
- `firmware/tests/test_home_font_hierarchy.py`
- `firmware/tests/test_murphy_default_exact_geometry.py` (intentional expectation updates only)
- `docs/orchestration/rounds/round-4-muse-impl.md` (required report)

Forbidden:

- `AppListActivity.*` (Luna), Settings hub/L2, FengyanTheme Home tab, unrelated dirty `M4ProviderCoverCache.*` (leave alone)
- Hardcoding snapshot probe sizes in production to green old tests
- `git push`, device flash, reset/clean of unrelated dirty files
- Do not USB-install the 49KB 晋江 package into a live QEMU SD (lessons: SerialException / SIGTERM). Prefer already-seeded plugin on SD, or stop QEMU → `mcopy` → restart `--skip-build`.

## Acceptance

### Luna (must finish P0 analysis even if no code change)

1. Reproduce mentally + with host evidence: drawer open 晋江 → what paints, who owns `displayBuffer`, mutex order, `subActivity` gate.
2. If root cause found: minimal patch; RED-then-green host test preferred.
3. `kDrawerColumns = 3` (or equivalent); retune tile height / icon / gaps / label font; keep `utf8EllipsizeChars(..., 4)`.
4. Commit on `agent/home-luna-audit` only. Message: `round-4(luna): <one line>`.
5. End report must say **花屏: root cause / fix / unverified** explicitly. If no smoking gun, write `round-4-luna-audit-BLOCKED.md` with ranked hypotheses — do **not** churn random diffs.

### Muse

1. **After** reading Luna’s 花屏 conclusion (or in parallel only on Home type/placeholder): apply assigned paint fix if any; do not duplicate Luna’s AppList mutex work.
2. Rebalance Home type (ids that actually paint: 12→UI_12, 14→NS14, 16→NS16, 18–23→NS18; id 25 blank).
3. Missing covers: visible placeholder, not empty white (hero + mini).
4. Host geometry/typography tests green for intentional changes.
5. QEMU proof (own session, detach recipe):
   - `PLATFORMIO_HOME_DIR=/tmp/pio_home2`
   - build `murphy_m4_qemu_plugin` once if needed
   - `./m4sim run --plugin-debug --skip-build --no-hostfwd --ready-seconds 20` (detach; never 120)
   - Home screenshot + open drawer + launch `com.jjwxc.client` (or tap 晋江) + capture **first plugin frame**
   - `m4adb screenshot` → `/usr/bin/python3` + PIL RGB; logical 480×800; QEMU panel may need `ROTATE_270`
6. Commit on `agent/home-muse-impl` only. Message: `round-4(muse): <one line>`.

## Hard bans (every lane)

- No `git push origin`. No hardware flash. No `git reset --hard` / `git clean`.
- Do not edit coordinator `feature/critical-ui-home` tree.
- No concurrent PlatformIO across lanes. Luna: no PIO. Muse: QEMU only in Muse worktree after host tests.
- No Codex Sol.
- English prompts / English lane reports. Human-facing summary is coordinator’s job (Chinese).

## CWD

```
/Volumes/z/paseo/workspaces/paseo/worktrees/041rfr5o/m4-home-muse-impl
/Volumes/z/paseo/workspaces/paseo/worktrees/041rfr5o/m4-home-luna-audit
```

Host toolchain: `/opt/homebrew/bin/g++-14`, `/opt/anaconda3/bin/pytest`, `/usr/bin/python3`. Write every `-I` flag; zsh will not split `$INC`.

## Coordinator after both finish

Merge Luna (P0) then Muse into `agent/home-orch-integration`, host re-run, single QEMU pass if needed, report to human in Chinese with local PNG markdown. No flash unless authorized.
