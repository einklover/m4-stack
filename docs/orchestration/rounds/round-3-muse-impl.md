# Round 3 — Muse impl (Lane A) — Settings Hub + L2 Scene

## Goal
Implement Settings Hub + L2 Scene packages and host-testable policy/model per `docs/superpowers/specs/2026-08-31-settings-l2-scene.md`. Minimal e-ink grammar: clear/text/line/battery/repeat + filled round_rect r=0 selected bar only. Locked chrome: title [24,24,280,24] ui_16_bold, battery [432,24,24,12], line (23,52)-(456,52). Hub repeat 432x140 gap12 limit4; L2 repeat 432x80 gap4 limit8; kMaxWindowRows/kSettingsL2Window=8.

## Files changed (allowlisted only)
- `themes/murphy-settings/hub.json` — minimal hub theme: clear white, static title “系统设置” [24,24,280,24] ui_16_bold, battery [432,24,24,12] $system.battery, line 1px, repeat $hub.cards limit4 24,68 432x140 gap12 vertical, child round_rect [0,20,4,100] r0 fill true visible_if $item.selected, child text [24,52,380,36] $item.title ui_20_bold. Bindings $hub.cards 71, $item.selected 75, $item.value 74, $item.is_section 76, $item.is_row 77; actions open_hub_card 40, activate_setting 41. No icon/cover/progress, no stroke.
- `themes/murphy-settings/l2.json` — minimal L2 theme: clear, title $page.title [24,24,280,24] ui_16_bold, same battery/line, repeat $page.rows limit8 24,68 432x80 gap4 vertical, children: round_rect [0,12,4,56] r0 fill true visible_if $item.selected, setting name [20,28,260,24] $item.title ui_16_regular visible_if $item.is_row, section name [20,32,400,20] $item.title ui_14_regular visible_if $item.is_section, value [280,30,140,20] $item.value ui_14_regular right visible_if $item.is_row. Same bindings (page.title 70, page.rows 72, etc.) and actions. No icon/cover/progress/stroke.
- `firmware/src/generated/murphy_settings_hub_m4theme.h` — compiled from hub.json (48380 bytes, CRC d8ae8377)
- `firmware/src/generated/murphy_settings_l2_m4theme.h` — compiled from l2.json (48420 bytes, CRC 54dfb97b)
- `firmware/src/activities/settings/SettingsHubPolicy.h` — ported adaptation grouping (kDisplayRows 19 incl frontlight/uiFontSize/readerLayout/pageTurn m4Only, kKeysRows 6, kNetworkRows 6, kSystemRows 7 incl developerOptions/switchBootSlot) plus new APIs exactly per spec §7: SettingsFlatKind/SettingsFlatRow, kSettingsHubCardCount=4, kSettingsL2Window=8, kSettingsContentTop=68 etc., settingsFlatCount/At/IndexOfSetting/settingsWindowStart/settingsNavSyncWindow/settingsNavOpenCard/Back/MoveHub/MoveRow (wrapping, skip sections, window clamped to contain flatIndex at bottom edge, maxStart flatCount-window). Also ported makeSettingsHubLayout/settingsHubCardFromPoint/settingsCategoryRowFromPoint and compatibility shims KeysOps/SystemMaint aliases, settingsNavActivateConfirm/ReturnFromPicker. Host-testable, no Arduino.
- `firmware/src/activities/settings/SettingsSceneModel.h` — new host-testable model (no Arduino/heap). kMaxWindowRows=8, text arena 2048, window 8 slots title/value/id/isSection/isRow/selected + pageTitle/battery/pane/hub. Copy of Home/Mock arena style: boundedLength/canAppend/appendText/UiStateStore<Snapshot>. Exposes bindingSource/count/resolve for hub (71→4 cards, $item.title 32, $item.id 30, $item.selected 75 bool pane==Hub && hub==index) and L2 (72→8 window, $item.title 32 text, $item.value 74 text, $item.selected 75 bool, $item.is_section 76 bool, $item.is_row 77 bool, $item.id 30 text, $page.title 70, $system.battery 1 int). Also populateHubFromPolicy/populateWindowFromPolicy helpers that use policy flatten + nav window (empty补空 → isRow false selected false). Action targets open_hub_card 40, activate_setting 41 (section not activatable).

## Geometry / minimal checks
- Hub repeat x24 y68 432x140 gap12 limit4 verified; L2 repeat x24 y68 432x80 gap4 limit8 verified.
- Title/battery/line chrome matches Home and spec §3.
- Selection bar filled round_rect r=0 fill:true visible_if $item.selected no stroke; tested no node has stroke>0 or icon/cover/progress via python check.
- Both JSON compile with `firmware/tools/compile_home_theme.py` without error; fill on round_rect accepted.

## Policy correctness (host)
- Display M4 flat 24 = 5 titles +19 settings; Keys 6 (single group no title); Network 8 =2 titles+6; System M4 9 =2 titles+7. Verified via `/tmp/driver` with g++-14.
- Window always 8: settingsWindowStart clamps to [0, flatCount-8], contains flatIndex at bottom edge, small lists windowStart 0.
- settingsHubContainsKey Display has readerLayout and never sdOta (scanned all cards).
- settingsNavSyncWindow after move keeps selected flat inside [windowStart, windowStart+8); tested 30-step walk over Display 19 settings.
- Ported hub IA still passes adaptation `test_settings_hub_ia.cpp` (titles, key membership, readerLayout before pageTurnAnimationFrameRate, Keys remapButtons first, Network/System checks, no fake keys).

## Model correctness (host)
- kMaxWindowRows 8 static_assert passes.
- Hub pane: battery 80 binding, hubCount 4, hub title, hub selected bool, title len checks.
- L2 pane: windowCount 8, pageRows size 8, row0 is_section true (“界面”), row1 is_row true selected true, windowStart for last setting (index 18) → 16 (flat 16..23), empty补空 rows isRow false.

## Commands run (worktree /Volumes/z/paseo/workspaces/paseo/worktrees/041rfr5o/m4-home-muse-impl)
```
git status --short --branch # prior dirty wifi/cover-cache files preserved, new lane files untracked
/opt/homebrew/bin/g++-14 -std=c++17 -fsyntax-only -I firmware/src firmware/src/activities/settings/SettingsHubPolicy.h
/opt/homebrew/bin/g++-14 -std=c++17 -fsyntax-only -I firmware/src firmware/src/activities/settings/SettingsSceneModel.h
cd firmware && /usr/bin/python3 tools/compile_home_theme.py --theme ../themes/murphy-settings/hub.json --out /tmp/murphy_settings_hub.m4theme --emit-header src/generated/murphy_settings_hub_m4theme.h
cd firmware && /usr/bin/python3 tools/compile_home_theme.py --theme ../themes/murphy-settings/l2.json  --out /tmp/murphy_settings_l2.m4theme  --emit-header src/generated/murphy_settings_l2_m4theme.h
/usr/bin/python3 /tmp/check_theme.py # chrome repeat limit stroke icon checks → PASS
/opt/homebrew/bin/g++-14 -std=c++17 -I firmware/src /tmp/driver.cpp -o /tmp/driver && /tmp/driver # flat counts 24/6/8/9, window, readerLayout, sdOta → ALL PASS
/opt/homebrew/bin/g++-14 -std=c++17 -I firmware/src /tmp/test_hub_ia.cpp -o /tmp/test_hub_ia && /tmp/test_hub_ia # adaptation IA still PASS
/opt/homebrew/bin/g++-14 -std=c++17 -I firmware/src /tmp/test_model.cpp -o /tmp/test_model && /tmp/test_model # scene model bindings PASS
```
No PlatformIO / QEMU per task bans; no hardware flash; no git push.

## Notes
- Section titles for flatten: Display “界面/墨水屏刷新/前光/阅读排版/翻页动画”, Network “网络/同步入口”, System “系统/维护”; Keys omits title per spec. titleZh for settings mirrors key (i18n resolved in Activity).
- Frontlight (section 2) and PageTurn (section 4) omitted on non-M4 builds when secCount 0.

## Activity Hub↔L2 (second commit, coordinator kick 2026-08-31)
- `firmware/src/activities/settings/SettingsActivity.h` — replaced 3-tab state (`categoryCount=3`, `selectedCategoryIndex`, `selectedSettingIndex`, 3 vectors) with Hub↔L2 model: `SettingsNavState navState_`, `SettingsScene::SettingsSceneModel sceneModel_`, 4 hub vectors (`displayReadingSettings_`, `keysSettings_`, `networkSettings_`, `systemSettings_`). Removed `categoryCount/categoryNames/enterCategory`. Added `rebuildModel()`, `currentHubSettings()`, `valueTextForSetting()`, `toggleCurrentSetting()`, `openHubCard()`, `handleHubConfirm/handleL2Confirm/handleL2TapIndex()`.
- `firmware/src/activities/settings/SettingsActivity.cpp` — full rewrite to Hub↔L2:
  - `onEnter`: buckets `getSettingsList()` via `settingsHubContainsKey` into 4 hub vectors, appends device-only ACTIONs per hub (`readerLayout`→Display, `remapButtons`→Keys head, `bluetooth/koreader/jianguo/dataCapsule`→Network, `clearCache/resetSettings/developerOptions/switchBootSlot`→System), no `sdOta`/`SdMan.exists("/update/firmware.bin")`, no `OtaUpdateActivity`/`CalibreSettingsActivity` include, adds `readerLayout`→`EpubReaderSettingsActivity` doorway with savedRow/savedHub restore and `settingsNavSyncWindow`, initializes `navState_` Hub DisplayReading and `rebuildModel()`.
  - `rebuildModel()`: `sceneModel_.begin(Ready)`, `setBattery(powerManager.getBatteryPercentage())`, `setPane/setHub`, Hub → `setPageTitle(kSystemSettings)` + `populateHubFromPolicy()`, L2 → `setPageTitle(settingsHubCardTitleZh)` + `clearWindow` + 8 `setWindowRow` with real `valueTextForSetting` (TOGGLE on/off, ENUM index bounds-checked, DynamicEnum via valueGetter, VALUE lineSpacing/refreshFrequency formatting, ACTION bluetooth `isEnabled` / switchBootSlot `runningOtaLabel`), section titles via `L(Str::kSection*)` (I18n), selected via `settingsFlatIndexOfSetting`, empty补空 rows isRow false.
  - `loop`: `pumpSubActivityFrame`, Back gesture/button: L2→Hub via `settingsNavBack`+rebuild, Hub→Home via `SETTINGS.saveToFile()+EpdFontLoader::loadFontsFromSd+onGoHome`, Hub `hasTouch` swipe paging via `M4ListTouchPolicy::applyPage` on `selectedRow` (window 8), physical keys: Hub Up/Down/Left/Right → `settingsNavMoveHub`, Confirm→`openHubCard`, L2 Up/Down/Left/Right → `settingsNavMoveRow`+`settingsNavSyncWindow`+rebuild, Confirm→`toggleCurrentSetting`, Touch: `hitTestScene` on `murphy_settings_hub_m4theme`/`murphy_settings_l2_m4theme` with `bindingSource(snap)`, Hub hit `open_hub_card` 40 → `openHubCard`, L2 hit `activate_setting` 41 → `handleL2TapIndex` (section ignored, setting → update selectedRow+sync+toggle), `touchDown` select-only without activate. Removed `changeTabsMs` long-hold tab switch, `settingsTabFromPoint`/`settingsRowFromPoint` +1 mapping.
  - `toggleCurrentSetting`: uses `navState_.selectedRow` against `currentHubSettings()`, TOGGLE/ENUM/ENUM Dynamic (with `applySystemChrome` for `uiFontSize`, LUT via `valueGetter`), VALUE (signed/unsigned) opens `NumberSelectionActivity` with savedRow/savedHub restore, ACTION per key (`remapButtons`→`ButtonRemapActivity`, `readerLayout`→`EpubReaderSettingsActivity`, `bluetooth`→`SimpleBluetoothActivity`, `koreader`→`KOReaderSettingsActivity`, `jianguo`→`JianGuoYunSettingsActivity`, `dataCapsule`→`DataCapsuleSettingsActivity`, `clearCache`→`ClearCacheActivity`, `resetSettings`→`ResetSettingsActivity`, `developerOptions`→`DeveloperOptionsActivity`, `switchBootSlot`→`esp_ota_set_boot_partition`+`ESP.restart` with popup, no `sdOta`), restores `navState_` and `rebuildModel` on return. Preserves `SETTINGS.saveToFile()` and LUT safety.
  - `render()`: `clearScreen`, `copyLatest(snap)` fallback, `UiSceneAssets` empty, `GfxSceneRenderer::render` Hub or L2 package, overlay **only** `GUI.drawButtonHints` (no `drawHeader`/`drawTabBar`/`drawList` second chrome).
- `firmware/src/I18n.h` — added hub titles (`kHubDisplayReading` “显示与阅读”/“顯示與閱讀” etc. 4), L2 section titles (`kSectionUiChrome` “界面”, `kSectionEinkRefresh` “墨水屏刷新”, `kSectionFrontlight` “前光”, `kSectionReaderDoor` “阅读排版”/“閱讀排版”, `kSectionPageTurn` “翻页动画”/“翻頁動畫”, `kSectionNetworkToggles` “网络”/“網絡”, `kSectionSyncDoorways` “同步入口”, `kSectionSystem` “系统”/“系統”, `kSectionMaintenance` “维护”/“維護”) and `kReaderLayout` “阅读排版”, no frozen category renames.
- Verified via `grep`: no `drawTabBar`, no `drawList`, no `sdOta`/`OtaUpdateActivity`/`SdOtaUpdater`/`updater_fw`, no `selectedSettingIndex`, no `categoryCount` in new files; `drawButtonHints` single overlay, `switchBootSlot` kept, `readerLayout` present, Hub↔L2 wiring via `murphy_settings_*_m4theme.h` + `SettingsHubPolicy`/`SettingsSceneModel`.
