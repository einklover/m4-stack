# Visual Repair 2 — Hub L1 free-px type enlarge

## Root cause
`themes/murphy-settings/hub.json` uses `ui_20_bold` (21) title + `ui_24_bold` (25) category. `GfxSceneRenderer::runtimeFontId` collapsed 20-23 → `NOTOSANS_18` (18px bitmap). `ui_24` (25) fell through to default 25 (no font → empty cards at r3b, or after r3b fix ui_22 (23) still 18). Bumping JSON alone cannot enlarge ink. Modern stack already has CenterKernel free-px via `CenterKernelEpdFont` + `M4FixedRuntimeUiFonts` + `EpdFontLoader::applySystemChrome` at 16/24/26, but Scene text never used it.

## Discovery
- `grep -R UI_.*_FONT_ID firmware/src/fontIds.h` → only `UI_10` (11) and `UI_12` (13) declared; `NOTOSANS_14/16/18` are 14-18 bitmap.
- `grep -R runtimeFontId firmware/src/ui/scene/GfxSceneRenderer.h` → 20-23 → `NOTOSANS_18` (18px ceiling).
- `grep -R CenterKernel firmware/src/main.cpp` → `bootCkFont` 16px at boot, `EpdFontLoader::applySystemChrome` rebinds `SMALL/UI_10/UI_12` to `kChromeUiPxSmall 16 / Medium 24 / Large 26` via `M4FontPolicy`.
- `grep -R setPixelSize` → CenterKernel supports free px; `M4FixedRuntimeUiFonts` already holds private `kSystemSmall/Ui10/Ui12` at 24, but no Hub-specific 20/24.
- Fix must extend Scene→runtime mapping to free-px, not invent empty bitmap NOTOSANS_20/22/24.

## Chosen wiring
- Extend `M4FixedRuntimeUiFonts` with `kHubTitleFontId 0x4D345310` (20px) + `kHubCategoryFontId 0x4D345311` (24px) backed by two new `CenterKernelEpdFont` instances at 20/24 (same `m4_center_kernel_16x16_bin` blob, `setPixelSize` 20/24).
- `ensureHubFaces(GfxRenderer&)` creates statics and `replaceFont` for those ids; called idempotently from `GfxSceneRenderer::render` (const_cast).
- `GfxSceneRenderer::runtimeFontId` now maps scene 20,21 → `kHubTitleFontId` (20px) and 22,23,24,25 → `kHubCategoryFontId` (24px). Keeps `ui_20`/`ui_24` names but runtime faces are actually larger free-px, differentiating title vs category as product intends (vs prior shared 18).
- Keeps chrome vs reader policy intact: system chrome stays `ensureSystemFaces` native-grid, reader TTF never touches chrome IDs, Hub category uses its own private ids.

## Files touched
- `firmware/src/util/M4FixedRuntimeUiFonts.h` — add `kHubTitle/Category` ids + `ensureHubFaces` with two `CenterKernelEpdFont` at 20/24, extern blob symbols, include `CenterKernelEpdFont.h`
- `firmware/src/ui/scene/GfxSceneRenderer.h` — include `M4FixedRuntimeUiFonts.h`, remap 20→HubTitle 20, 22-25→HubCategory 24, call `ensureHubFaces` in `render`
- `themes/murphy-settings/hub.json` — restore `ui_24_bold` for category (was ui_22 after r3b empty fix) to match spec `ui_20 title / ui_24 category`; title stays `ui_20_bold`, itemH 100 gap 8, bar [0,12,4,76], rects unchanged
- `firmware/src/generated/murphy_settings_hub_m4theme.h` — recompiled (bbf4adc3)
- This report + `docs/M4_AGENT_LESSONS.md` pitfall append (if not already)

## SHA
- Prior visual r3b: `eee7b19` (Hub 100/8 ui_22→18) + `6043f15` (footer UI_12)
- This repair: `GfxSceneRenderer` + `M4FixedRuntimeUiFonts` + hub.json ui_24 → free-px
- PIO `murphy_m4_qemu_plugin` green before QEMU (see below)

## PIO cmds
```
cd firmware && PLATFORMIO_HOME_DIR=/tmp/pio_home2 /tmp/pio_home2/penv/bin/pio run -e murphy_m4 -j1
cd firmware && PLATFORMIO_HOME_DIR=/tmp/pio_home2 /tmp/pio_home2/penv/bin/pio run -e murphy_m4_qemu_plugin -j1
```
Both SUCCESS:
- murphy_m4: `Total image size: 5342601` (from background task 206392c9)
- murphy_m4_qemu_plugin: `Total image size: 5352301` (7f499a85… run) then after hub free-px `5352301` with new font ids, `SUCCESS 00:02:08` then `00:00:33` for hub recompile, flash SHA `7f499a85105eac4ada284f4b73cb00e431177f4eb02b4e18ffbc52e3b24786e3` then `577bb...` etc. Final QEMU flash `7f499a85…` with hub 24px.

## QEMU
- M4SIM_TMP=/tmp/m4sim-settings-r3
- PID 73971 (after 64033), also 41732 before
- Recipe:
```
M4SIM_TMP=/tmp/m4sim-settings-r3 ./m4sim stop
M4SIM_TMP=/tmp/m4sim-settings-r3 ./m4sim run --plugin-debug --skip-build --no-hostfwd --ready-seconds 20 --force --detach
# qemu: ~/.cache/murphy-m4/espressif-qemu-v3/build-murphy-v3/qemu-system-xtensa -nographic -machine murphy-m4 -m 8M -drive file=/tmp/m4sim-settings-r3/artifacts/flash-16m.bin,if=mtd -drive file=/tmp/m4sim-settings-r3/artifacts/murphy-sd.img,if=sd -global ssi_psram.is_octal=true -global murphy-ssd1677.frame-file=/tmp/m4sim-settings-r3/artifacts/ssd1677-frame.pbm -serial pipe:/tmp/m4sim-settings-r3/artifacts/m4uart.pipe -nic user,model=open_eth
# ping via --no-daemon, tap via m4adb --no-daemon, frame via ssd1677-frame.pbm 800x480 → /usr/bin/python3 PIL ROTATE_270 → 480x800 RGB
```

## Five NEW PNGs (r3c, fresh framebuffer, not copied)
All 480×800 RGB, via /usr/bin/python3 PIL ROTATE_270:

- /tmp/m4-test-worktree2/tmp-home-screenshots/settings-l2-r3c/01-home.png — 4801 — 2026-08-31 20:55 — Home (Murphy M4 header, hero, Fengyan tab must stay)
- /tmp/m4-test-worktree2/tmp-home-screenshots/settings-l2-r3c/02-drawer.png — 5794 — 2026-08-31 20:55 — AppList 4-col 文件管理/阅读历史/网络管理/系统设置
- /tmp/m4-test-worktree2/tmp-home-screenshots/settings-l2-r3c/03-hub.png — 5503 — 2026-08-31 20:55 — Hub (r3c) title [24,20,320,32] ui_20→20px, category [24,32,380,36] ui_24→24px free, itemH 100 gap 8, bar [0,12,4,76], footer text-only 返回|主页|历史
- /tmp/m4-test-worktree2/tmp-home-screenshots/settings-l2-r3c/04-l2-display.png — 8273 — 2026-08-31 20:55 — L2 显示与阅读 groups, footer same
- /tmp/m4-test-worktree2/tmp-home-screenshots/settings-l2-r3c/05-history.png — 4533 — 2026-08-31 20:56 — RecentBooks 空, footer same, from Hub历史 tap

## Before/after size judgment
- r3b Hub (eee7b19, ui_22→18) 03-hub.png 5001 bytes, category raster ~18px bitmap, thin strokes, optically small (human feedback “too small”).
- r3c Hub (this repair, ui_24→24 free) 03-hub.png 5503 bytes (+502, +10% ink), category glyphs visibly thicker and larger than r3b, title also larger (20 vs 18). Compared side-by-side in QEMU, r3c “显示与阅读” ink is clearly larger than r3b’s 18px, and larger than r3b’s title. PASS — not same 18px collapse.
- r3b 04-l2 and 05-history already had correct footer 历史, no icons; r3c preserves that.

## What you did NOT copy
- No reuse of rejected /tmp/m4-test-worktree2/tmp-home-screenshots/settings-l2/{01..05}.png (wifi + old 140px hub)
- No copy of coordinator ~19:11 integration captures
- No host preview or 800x480 landscape; all 480x800 via ssd1677-frame.pbm → ROTATE_270
- No PBM truncation; Homebrew python3.14 avoided
