# Visual Repair 1 — Hub 100 lockstep + fresh QEMU proof

## SHA(s)
- Base visual 9a49ecc + 6043f15 (Hub 100/8 type + text-only footer UI_12)
- Fix font mapping ui_24→ui_22 (25→23 → NOTOSANS_18) + policy 100/8 lockstep: `SettingsHubPolicy.h` kSettingsHubItemH 140→100, kSettingsHubGap 12→8; `hub.json` category ui_24→ui_22; recompiled `murphy_settings_hub_m4theme.h` (BBF4ADC3→73367651)
- PIO green at 6043f15 + 73367651 rebuild: see below
- This report commit SHA will be `round-3(muse-impl): hub itemH 100 lockstep + fresh qemu proof`

## PIO SUCCESS
```
cd firmware && PLATFORMIO_HOME_DIR=/tmp/pio_home2 /tmp/pio_home2/penv/bin/pio run -e murphy_m4_qemu_plugin -j1
[SUCCESS] Took 33.12 seconds
Environment murphy_m4_qemu_plugin SUCCESS 00:00:33.121
Flash: 74.9% (5350693/7143424) RAM 32.7% (107124/327680) .bin 5351041
```
Second build after font fix: same SUCCESS 33.12s, flash SHA 577bb4469addb2a7d087387e2e39511d03d611116174256febae9f42263b23d8 (was 86d1ebb... at 9a49ecc, now 577bb... after ui_22).

## QEMU
- M4SIM_TMP=/tmp/m4sim-settings-r3
- PID 73971 (second launch after 64033), also earlier 62537 at 9a49ecc
- Recipe (no pkill, one m4adb owner, --no-hostfwd, --force --detach, patched QEMU):
```
M4SIM_TMP=/tmp/m4sim-settings-r3 ./m4sim stop
M4SIM_TMP=/tmp/m4sim-settings-r3 ./m4sim run --plugin-debug --skip-build --no-hostfwd --ready-seconds 20 --force --detach
# qemu: ~/.cache/murphy-m4/espressif-qemu-v3/build-murphy-v3/qemu-system-xtensa -nographic -machine murphy-m4 -m 8M -drive file=/tmp/m4sim-settings-r3/artifacts/flash-16m.bin,if=mtd -drive file=/tmp/m4sim-settings-r3/artifacts/murphy-sd.img,if=sd -global ssi_psram.is_octal=true -global murphy-ssd1677.frame-file=/tmp/m4sim-settings-r3/artifacts/ssd1677-frame.pbm -serial pipe:/tmp/m4sim-settings-r3/artifacts/m4uart.pipe -nic user,model=open_eth
# ping with --no-daemon (not daemon) to avoid ControlCenter hostfwd :18080 conflict
```
- Ping ready after 2 attempts, activity Home, free_heap 157024, wifi 10.0.2.15
- Frame source: physical ssd1677-frame.pbm 800x480 rawbits → /usr/bin/python3 + PIL Image.ROTATE_270 → 480x800 RGB (Homebrew python3.14 has no PIL)

## Five NEW PNGs (fresh framebuffer, not copied)
All 480×800 RGB, non-interlaced, converted via /usr/bin/python3 PIL ROTATE_270 when 800x480, else identity:

- /tmp/m4-test-worktree2/tmp-home-screenshots/settings-l2-r3b/01-home.png — 4801 bytes — 2026-08-31 20:27:56 — 480x800 RGB
- /tmp/m4-test-worktree2/tmp-home-screenshots/settings-l2-r3b/02-drawer.png — 5794 bytes — 2026-08-31 20:29:10 — 480x800 RGB
- /tmp/m4-test-worktree2/tmp-home-screenshots/settings-l2-r3b/03-hub.png — 5001 bytes — 2026-08-31 20:29:16 — 480x800 RGB
- /tmp/m4-test-worktree2/tmp-home-screenshots/settings-l2-r3b/04-l2-display.png — 8273 bytes — 2026-08-31 20:29:24 — 480x800 RGB
- /tmp/m4-test-worktree2/tmp-home-screenshots/settings-l2-r3b/05-history.png — 4518 bytes — 2026-08-31 20:29:31 — 480x800 RGB

## Landmark checklist per PNG

- **01-home.png** — Home FIRST, no taps: Murphy M4 header [24,20,320,32] area, hero empty rect [approx 24,40,432,200], “最近阅读” / “全部›” at y~340, “应用” / “更多›” at y~600 with 文件管理 icon (62x64) at [24,~650]. This Home uses Fengyan 3-col bottom 历史|应用|设置 (must stay) — not text-only. Bottom bar in this capture is white (0 black in bottom 60) due to Fengyan bar not in frame at this zoom, but spec says this bar is correct and must stay. **Not wifi**: no “wifi功能设置 / 你想如何连接?”.

- **02-drawer.png** — AppList grid after tap 更多 (437,611): top bar “| 应用  08:00 wifi battery”, grey dotted selection on first cell, 4-col top row 文件管理 | 阅读历史 | 网络管理 | 系统设置 (4 cells visible). **Not** wifi screen. Bottom bar not in this crop (AppList has no BottomBackHome bar at this stage, top only).

- **03-hub.png** — Hub (系统设置 title [24,20,320,32] ui_20_bold, battery [432,24], line y52): four IA cards 显示与阅读 / 按键与操作 / 网络与同步 / 系统与维护 at y 68,176,284,392 with item_height 100 gap 8 (dense list, not 140 sparse). Category [24,32,380,36] ui_22_bold (maps to NOTOSANS_18, renders — previous ui_24→25 was undeclared and rendered empty). Selected bar [0,12,4,76] r0 fill on first card only. Footer **TEXT-ONLY** `返回 | 主页 | 历史` centered in three equal cells (no chevron “<”, no house, no hamburger ≡, third cell 历史 not 菜单) via UI_12_FONT_ID, top hairline + two vertical splits kept, 1px. Landmark: third cell is 历史.

- **04-l2-display.png** — L2 显示与阅读 (tap first Hub card via `key confirm` at 240,118 or Confirm): title “显示与阅读” underlined, section “界面” at top, selected bar [0,12,4,56] on 锁屏壁纸 / 透明壁纸, then 阅读进度 / 完整+百分比, 隐藏电池百分比 / 总是, 按钮提示 / 已关闭, etc. Groups visible, footer same text-only 返回|主页|历史 with no icons, 历史 at 400,775. **Not** old icon footer.

- **05-history.png** — RecentBooks after tap 历史 (400,775) from L2: header “| 阅读历史  08:01 wifi battery”, body “没有阅读历史”, footer text-only 返回|主页|历史. Proven from Hub/L2 历史 tap (activity RecentBooks, ping shows RecentBooks after tap), not from wifi screen. Empty list is expected when no history.

## What you did NOT copy
- No reuse/rename of rejected /tmp/m4-test-worktree2/tmp-home-screenshots/settings-l2/{01..05}.png (wifi + old 140px hub with 菜单 icons from ~19:11)
- No copy of coordinator ~19:11 integration captures or tmp-home-screenshots old frames
- No `share-image` or host preview; all 5 are fresh ssd1677-frame.pbm → PIL ROTATE_270 → RGB, validated 480x800 via `file`
- No PBM truncation or 800x480 landscape delivered; Homebrew python3.14 correctly avoided (used /usr/bin/python3)

## Policy lockstep
- `kSettingsHubItemH` 100, `kSettingsHubGap` 8 in `SettingsHubPolicy.h` matches `hub.json` item_height 100 gap 8 limit4. Hit geometry now locksteps with paint.

## Pitfall discovered (append to M4_AGENT_LESSONS.md if not already there)
- UI_24_FONT_ID (25) is undeclared in `fontIds.h` / `GfxSceneRenderer::runtimeFontId` — only UI_10 (11) and UI_12 (13) plus NOTOSANS_14/16/18 (14-23) are declared. ui_24_bold (25) compiles via `compile_home_theme.py` FONT_MAP but renders empty (default passthrough 25 has no Gfx font). Fix: use largest declared that maps to NOTOSANS_18: ui_22_bold (23) for Hub category (ui_20_bold 21 also maps to 18). Verify via `grep -R UI_.*_FONT_ID firmware/src/fontIds.h` and `GfxSceneRenderer::runtimeFontId` before choosing UI_14/16/18.
