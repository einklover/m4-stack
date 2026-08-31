# Round 3 — Settings Hub + L2 Scene (minimal, no icons/borders)

Coordinator: Grok. Worktrees already exist. Latest integration is merged into each lane **before** you start (see prompt cwd).

Product (user, 2026-08-31): design Settings secondary pages in the same minimal Home Scene grammar — **no icons, no borders** — reuse the new Scene node classes, then implement.

Binding spec (read first):

`docs/superpowers/specs/2026-08-31-settings-l2-scene.md`

Wireframes (480×800, authoritative look, not ChatGPT mock IA):

- `docs/orchestration/assets/settings-hub-wireframe.png`
- `docs/orchestration/assets/settings-l2-display-wireframe.png`

IA grouping source (port, do not invent new keys):

`git show feature/critical-ui-adaptation:firmware/src/activities/settings/SettingsHubPolicy.h`

Older IA spec (content grouping still valid; **outlined cards are rejected**):

`git show feature/critical-ui-adaptation:docs/superpowers/specs/2026-08-28-m4-settings-ia-design.md`

Home chrome to copy: `themes/murphy-default/theme.json` title `[24,24]` `ui_16_bold`, battery `[432,24,24,12]`, line y=52.

Do **not** copy `themes/settings-scene-mock/theme.json` into production (it has round_rect stroke + icon toggles + limit 5).

## File ownership (no overlap)

| Lane | Model | worktree | branch | May edit |
|---|---|---|---|---|
| A Muse impl | Muse Ultra | `m4-home-muse-impl` | `agent/home-muse-impl` | `themes/murphy-settings/hub.json`, `themes/murphy-settings/l2.json`, generated `firmware/src/generated/murphy_settings_hub_m4theme.h` + `murphy_settings_l2_m4theme.h`, **create** `firmware/src/activities/settings/SettingsHubPolicy.h`, **create** `firmware/src/activities/settings/SettingsSceneModel.h`, round report |
| B Luna | Luna Max | `m4-home-luna-audit` | `agent/home-luna-audit` | `firmware/src/activities/settings/SettingsActivity.cpp`, `SettingsActivity.h`, `firmware/src/I18n.h` (hub + section + `kReaderLayout` strings only), `firmware/src/util/TouchHitGeometry.h` **only** if a 0-based helper is required and not already in policy; round report |
| C Muse tests | Muse Ultra | `m4-home-muse-tests` | `agent/home-muse-tests` | **New files only** under `firmware/tests/**` and `simulator/tests/**`; round report. **Do not `git add` any pre-existing test file.** |

## Hard bans (every lane)

- No `git push origin`. No hardware flash. No `git reset --hard` / `git clean`.
- Do not edit `feature/critical-ui-home` or the coordinator worktree.
- No concurrent PlatformIO. **Do not run PIO. Do not run `./m4sim`. Do not touch QEMU.**
- No Codex Sol.
- Do not raise `kMaxRepeatItems` / `kMaxSceneNodes`.
- Do not mutate `FengyanTheme::drawTabBar` or `drawList`.
- Do not change `SettingsLists.h` category strings. Do not insert `readerLayout` into `getSettingsList()`.
- Do not reintroduce `sdOta`.
- Do not hardcode snapshot probe sizes in production.
- Commit only your lane files. Message: `round-3(<lane>): <one line>`.
- End with `docs/orchestration/rounds/round-3-<lane>.md`. If blocked, write `round-3-<lane>-BLOCKED.md` and stop.

Host toolchain:

- `/opt/homebrew/bin/g++-14` (not Apple clang++)
- `/opt/anaconda3/bin/pytest`
- `/usr/bin/python3` for PIL / compiler
- Write every `-I` flag; zsh will not split `$INC`
- CWD must be this lane worktree absolute path

```
/Volumes/z/paseo/workspaces/paseo/worktrees/041rfr5o/m4-home-muse-impl
/Volumes/z/paseo/workspaces/paseo/worktrees/041rfr5o/m4-home-luna-audit
/Volumes/z/paseo/workspaces/paseo/worktrees/041rfr5o/m4-home-muse-tests
```

---

## Lane A — Muse impl (theme + policy + model)

Implement the spec APIs **exactly** (names in spec §7). Port grouping from adaptation `SettingsHubPolicy.h`, then add flatten + window.

Hub JSON / L2 JSON geometry is locked in spec §3. Selection tick is filled `round_rect` `r=0` `fill: true` with **no stroke**. `visible_if` on `$item.selected` / `$item.is_section` / `$item.is_row`.

Compile:

```
cd <this worktree>/firmware
/usr/bin/python3 tools/compile_home_theme.py \
  --theme ../themes/murphy-settings/hub.json \
  --out /tmp/murphy_settings_hub.m4theme \
  --emit-header src/generated/murphy_settings_hub_m4theme.h
/usr/bin/python3 tools/compile_home_theme.py \
  --theme ../themes/murphy-settings/l2.json \
  --out /tmp/murphy_settings_l2.m4theme \
  --emit-header src/generated/murphy_settings_l2_m4theme.h
```

If the compiler rejects `fill` on `round_rect` or a binding name, fix the JSON to the nearest legal form that still paints a 4px filled bar — do not add stroke cards.

`SettingsSceneModel.h` is host-testable (no Arduino). Window slot count 8. Copy snapshot/text-arena style from `firmware/src/ui/pages/SettingsSceneMockModel.h` but do not include that mock from production headers.

Do **not** edit `SettingsActivity.cpp/.h`. Do **not** edit tests. Do **not** edit Home `theme.json`.

Self-check (header-only g++):

```
/opt/homebrew/bin/g++-14 -std=c++17 -fsyntax-only \
  -I firmware/src \
  firmware/src/activities/settings/SettingsHubPolicy.h
```

Plus a tiny `/tmp` driver you do **not** commit, asserting Display M4 flat count 24, Keys 6, Network 8, System M4 9, window always 8, `settingsHubContainsKey` has `readerLayout` on Display and never `sdOta`.

---

## Lane B — Luna (SettingsActivity lifecycle)

Replace 3-tab Settings with Hub↔L2 using Muse's headers. If those headers are not in your tree yet, still write against the spec §7 API; include the paths in spec. Do not invent a second policy.

Must:

- Delete tab-focus (`selectedSettingIndex==0`), `GUI.drawTabBar`, `categoryCount=3` paint path.
- Render hub or l2 package through `GfxSceneRenderer` (same path Home uses). Overlay **only** `GUI.drawButtonHints`. Do not draw a second header if the scene already has the title.
- Keep existing toggle/enum/value/ACTION behavior, including `applySystemChrome` on `uiFontSize` and LUT-safe `pageTurnAnimationFrameRate`.
- Add `readerLayout` doorway → `EpubReaderSettingsActivity`; restore this L2 row on the way back.
- Remove every `sdOta` append/branch. Keep `switchBootSlot`.
- Back: L2→Hub (restore selected card), Hub→Home (existing save + font reload).
- Touch: scene hit-test / policy geometry; no `settingsRowFromPoint` +1 tab mapping.
- I18n: Simplified + Traditional for the 4 hub titles, section titles used on L2, and `kReaderLayout`. Do not rename frozen catalog strings.

Do **not** edit theme JSON, generated m4theme headers, `SettingsHubPolicy.h` (Muse owns it), Fengyan list/tab, `SettingsLists.h`, web server, tests, PIO, QEMU.

If `SettingsActivity.cpp` compile needs a one-line include of the new generated header, that include is allowed. If Muse files are missing, write the include anyway and note it in the report.

---

## Lane C — Muse tests (new files only)

Create tests that will be RED until A/B land; that is OK. Do not patch production to make them green. Do not append old tests.

Suggested new files (names may vary, but stay new):

- `firmware/tests/native_app/test_settings_hub_ia.cpp` — key membership per card; `readerLayout` in Display; no `sdOta`; frozen hub titles.
- `firmware/tests/native_app/test_settings_l2_window.cpp` — flat counts; window size 8; selected setting always inside window; section rows not selectable via `settingsNavMoveRow`.
- `firmware/tests/test_settings_theme_minimal.py` — load both JSON files; fail if any node type is `icon`/`cover`/`progress`; fail if any `stroke` > 0; fail if L2 repeat limit != 8; assert chrome rects from spec §3.
- `firmware/tests/native_app/test_settings_activity_no_tabs.cpp` **source-scan only** of `SettingsActivity.cpp` for `drawTabBar` and `sdOta` (string contract). If the file still has them, test is RED — do not edit SettingsActivity.

Host compile like existing `test_home_composition_policy.cpp` / `test_settings_scene_reuse.cpp`. Do not require QEMU. Do not `git add` `test_settings_scene_reuse.cpp`.

---

## After you finish

Commit on your lane branch only. Write the round report. Stop. Coordinator merges and runs QEMU.
