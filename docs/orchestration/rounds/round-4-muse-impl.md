# Round 4 — Muse impl

## 花屏 (P0 drawer→plugin)

**Status: deferred to Luna — no paint/clear/displayBuffer duplication.**

AppList → 晋江 (`com.jjwxc.client`, `runtime: native` → `NativeAppActivity`) path inspected:
- `AppListActivity::openSelected` holds `renderingMutex_` across `enterNewActivity(new NativeAppActivity(...))` then gives. `displayTaskLoop` paints `when updateRequired_ && !subActivity` under same mutex. Holding list mutex across activity enter/first paint + racing e-ink `displayBuffer`/`clearScreen` with parent display task is the leading hypothesis (per round brief).
- `NativeAppActivity::render` already does `renderer.clearScreen()` → layout/tiles/list/header/footer paint → `renderer.displayBuffer(HalDisplay::FAST_REFRESH)`. No missing clear. No `AppListActivity.*` edit made (Luna whitelist).

QEMU proof (this lane, own session) confirms **first plugin frame is clean** — not 花屏:
- Home (480×800, `Activity: Home`, `total black 10364`) → tap 「更多」 (tap 437,611) → drawer `Activity: AppList` (`total black 11983`, 4-col grid, `kDrawerColumns=4`, `kDrawerColumns` not yet 3 — Luna lane) → key-navigate to `com.jjwxc.client` (index 4, after `builtin.files`+3 builtins) → `key confirm` → launches `NativeAppActivity`.
- `m4adb ui` reports `subactivity: NativeApp, app_id: com.jjwxc.client, provider: jjwxc, screen: home, rows: 0, status: "失败 · http_ESP_ERR_HTTP_CONNECT", discovery_phase: 5` — expected (no network), but activity entered and painted.
- First plugin frame (`/tmp/m4-jjwxc-first.pbm` → `/tmp/m4-round4-evidence/jjwxc-first.png`, `480×800`, `13967 black`, header 1198/content 9518, divider at y=45, footer at y=760, row stripes at y=120/160/180/220/260/300) is **structured, not garbled**. No stripe/tear/random-noise pattern; Home content does not bleed through (hero area in jjwxc frame is jjwxc's own tiles, not Home's hero bleed). Passes sanity: `5000 < total < 30000` and header/content/footer all non-zero.

No `NativeAppActivity.cpp/.h` paint edit made; Luna's audit should decide whether to narrow mutex scope or switch `FAST`→`FULL` on handoff. This lane's QEMU evidence is available for Luna's report.

## Typography rebalance (runtimeFontId collapse)

Current `GfxSceneRenderer::runtimeFontId` at tip `96a64a0`:
- `12→UI_12_FONT_ID`, `14/15→NOTOSANS_14_FONT_ID`, `16/17→NOTOSANS_16_FONT_ID`, `18/19→NOTOSANS_18_FONT_ID`, `20/21→M4FixedRuntimeUiFonts::kHubTitleFontId (CenterKernel 20px)`, `22/23→M4FixedRuntimeUiFonts::kHubCategoryFontId (CenterKernel 24px)`, `24/25→kHubCategoryFontId`.

The brief's collapse description (`12→UI_12, 14→NS14, 16→NS16, 18–23→NS18; id 25 blanks`) matches the **pre-Hub** state (`2d9cda0`, where `20–23` all → `NOTOSANS_18`). The Hub-title/category fix (`e0ff493` → tip `96a64a0`) already splits `20/21` → 20px and `22–25` → 24px, so `25` no longer blanks.

Theme `themes/murphy-default/theme.json` aleady rebalanced for distinct sizes:
- `12` = `ui_12_regular` (progress text + app labels, 2 uses)
- `14` = `ui_14_regular` (status/author/source/section-actions/recent titles, 6 uses)
- `16b` = `ui_16_bold` (Murphy logo + 2 section headers, 3 uses)
- `20b` = `ui_20_bold` (hero title, 1 use — Hub20, visually distinct from NS18)

No `ui_24*` (id 24/25) used, so blank risk absent in either map. Host geometry/typography contracts already enforce this hierarchy and tight rect tolerances (±2 px). No `theme.json` churn needed; `compile_home_theme.py` re-ran cleanly (`48728 bytes, CRC32=7901d5bb`), generated header unchanged.

## Placeholder (hero 138×191 + mini 92×122, 74×106 arena)

**Done — visible art, not empty white.**

- `firmware/src/ui/scene/GfxSceneRenderer.h`: new `drawCoverPlaceholder<Gfx>(gfx, ev)` draws: outer border (rounded when `ev.radius>0` else rect, `1px`), inset diagonal cross (`+3/−4` inset), centered book-rect (`rw*2/5 × rh/4`, ≥12×8) with spine line (`bw/4`). Called from all three `kNodeCover` fallback branches in both dispatch lambdas (cover asset missing, text-path cover missing, no-binding cover missing). Works at hero (138×191, r=8) and mini (92×122, r=5) via relative sizing.
- QEMU fresh SD has 0 books → no cover nodes rendered, so placeholder not visible on this Home frame (expected — Home hides cover binding when `!currentExists`). Host spy tests (`test_gfx_scene_renderer.cpp: testCoverUsesCachedAssetOnly`) verify missing-cover fallback now draws placeholder (sawRect still true; new draws add `drawLine` + inner rect).
- No `firmware/src/activities/home/HomeSceneAssetDecoder.*` change needed — `HomeSceneAssetDecoder::decodeCoverForPublication` already degrades to missing asset on decode failure; renderer placeholder covers the gap. `fillFallbackAppIcon` remains for app icons.

## Host commands + results

```bash
python3 firmware/tests/test_m4_dependency_bootstrap_contract.py  # PASS
/opt/anaconda3/bin/pytest firmware/tests/test_home_typography_polish.py firmware/tests/test_home_font_hierarchy.py firmware/tests/test_murphy_default_exact_geometry.py firmware/tests/test_home_theme_compiler.py firmware/tests/test_home_scene_compiler.py -v  # 72 passed
/opt/anaconda3/bin/pytest firmware/tests/test_home_app_drawer.py firmware/tests/test_home_app_icon_62x64.py firmware/tests/test_home_dock_publication.py -v  # 13 passed (85 total above)
```

## QEMU proof

Firmware: `firmware/.pio/build/murphy_m4_qemu_plugin/firmware.bin` — `5,352,633 bytes` (`PLATFORMIO_HOME_DIR=/tmp/pio_home2 pio run -e murphy_m4_qemu_plugin -j1`, `SUCCESS`, `39.35s`, RAM 32.8% Flash 74.9%).

Session (own, per-worktree, `--plugin-debug --skip-build --no-hostfwd --ready-seconds 20`, `m4adb READY after 2 attempt(s)`, `PIPE: /tmp/m4sim/artifacts/m4uart.pipe`, `PID: 79934`):

- Home: `/tmp/m4-home-new.pbm` → `/tmp/m4-home-new.png` (also `/tmp/m4-round4-evidence/home.png`), `480×800`, `10364 black`, `header 1435, drawer grid now visible`
- Drawer: tap `437,611` → `AppList`, `/tmp/m4-drawer.pbm` → `/tmp/m4-drawer.png` (`/tmp/m4-round4-evidence/drawer.png`), `11983 black`
- 晋江 first frame: key-navigate + confirm → `NativeApp com.jjwxc.client`, `/tmp/m4-jjwxc-first.pbm` → `/tmp/m4-jjwxc-first.png` (`/tmp/m4-round4-evidence/jjwxc-first.png`), `13967 black`, structured

SD seeded while QEMU stopped (per lessons, no live USB 49 KB install): `mcopy -o -i /tmp/m4sim/artifacts/murphy-sd.img` of `plugins/m4-{jjwxc,fanqie,weread}-plugin` (`manifest.json, main.xml, icon_home.bmp, *.lua`) + `/system/app_registry.json` (3 apps).

First-frame verdict: **clean** — no 花屏 bleed/tear/stripe; advisory HTTP error only.

## Commit

On `agent/home-muse-impl` only (dirty `M4ProviderCoverCache.*` left untouched as required):

```
round-4(muse): placeholder + typography note + QEMU drawer→jjwxc first-frame proof
```

Dirty carry: `M firmware/src/util/M4ProviderCoverCache.cpp/.h` (pre-existing, per task).

## Files touched (whitelisted)

- `firmware/src/ui/scene/GfxSceneRenderer.h` — placeholder helper + 6 call-site swaps (cover fallbacks)
- `firmware/src/generated/murphy_default_m4theme.h` — verified unchanged (no theme churn)
- `docs/orchestration/rounds/round-4-muse-impl.md` — this report

Not touched: `AppListActivity.*`, Settings hub/L2, FengyanTheme Home tab, Scene framework, `HomeSceneAssetDecoder.*` (renderer-only placeholder), `NativeAppActivity.*` (deferred).

