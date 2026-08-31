# Round 8 — Muse: M4 Settings device compatibility audit (3-left-buttons + touch)

Date: 2026-09-01  
Workspace: `/Volumes/z/paseo/workspaces/paseo/worktrees/041rfr5o/m4-home-muse-impl`  
Auditor: Muse Ultra (`muse-spark-1.2`)  
Device: Murphy M4 — **three physical buttons on the LEFT + capacitive touchscreen** (tap / swipe / edge gestures).  
Spec baseline: `docs/superpowers/specs/2026-08-31-settings-l2-scene.md` + `SettingsHubPolicy.h` (Hub/L2 IA), `SettingsLists.h`, `MappedInputManager`, `HalGPIO.h`, `simulator/board/murphy_m4.json`

> Primary deliverable = **REPORT ONLY** (no code changes, no flash, no push). Every `hide`/`rewrite` row cites a `file:symbol` where the key is consumed or proved dead on M4. QEMU evidence is noted where it helped disambiguate orientation/touch; no claim rests on QEMU alone.

---

## 1. Executive summary

On Murphy M4 the **only physical inputs are `Up(GPIO1)`, `Down(GPIO2)`, `Power(GPIO0)`** on the left edge (`simulator/board/murphy_m4.json:64-68`, `simulator/hardware/SimInput.h:10`). There are **no front/side hardware `Back/Confirm/Left/Right`** buttons. The four logical buttons `Back / Confirm / Left / Right` live as **bottom-bar touch slots** (`MappedInputManager.cpp:219-233` `M4FooterTouchPolicy::slotFromPoint` + `HalGPIO.h:84-92` defines `BTN_BACK=0 … BTN_RIGHT=3` for logical remap only). Edge-swipe `Back` and bottom-swipe `Home` are unified with those slots via `MappedInputManager::wasBackGesture` / `wasHomeGesture` (`MappedInputManager.cpp:449,491`).

95% of the L2 IA is sound. The incompatibilities are concentrated in:

- **Display chrome leftovers** `iconStyle` / `homeIconStyle` that assumed the old non-Scene Home icon pipeline — `homeIconStyle` is fully dead on the current Scene Home; `iconStyle` only survives for the file-manager 24/32 icons, not for Home's plugin BMPs.
- **Controls/Keys wording** that still assumes *front hardware* or *two-sided power/confirm* gestures — all six Keys rows remain functional but five need label/option rewrites to match **left-3 + bottom-touch**.
- No hidden X3 sensor (tilt/knock) leaks into the M4 Hub (correctly `#ifdef CROSSPOINT_X3`-gated in `SettingsLists.h:177-224`).
- No Bluetooth stub — `BluetoothHIDManager` is a real NimBLE HID host (`firmware/lib/hal/BluetoothHIDManager.h:41-56`), correctly exposed as `bluetooth` ACTION.
- `landscapeDualPage` is **intentionally not on the M4 Hub** (lives in `SettingsLists.h:156` category `Reader`, web/API-only). Its runtime only fires when `orientation==LANDSCAPE_CCW` (`EpubReaderActivity.cpp:50` / `TxtReaderActivity.cpp:60`), which itself is entered via a 1 s hold of `PageBack(Up)` (`EpubReaderActivity.cpp:542-572`). Keeping it off Hub is correct per spec §4.1.

Verdict counts (38 device-Hub rows, M4 build):

| hub | keep | rewrite | hide | decide |
|-----|------|---------|------|--------|
| DisplayReading (19) | 14 | 1 | 2 | 1 |
| KeysOperations (6) | 1 | 5 | 0 | 0 |
| NetworkSync (6) | 6 | 0 | 0 | 0 |
| SystemMaintenance (7) | 7 | 0 | 0 | 0 |
| **total** | **28** | **6** | **2** | **1** |

The two `hide` and one `decide` are Display chrome, not input safety, so a small optional diff would be safe. **Per-task rule: report-only; no code diff committed** (see §7 for the exact hide gate that would be used if product confirms).

---

## 2. Device facts (source-proved)

| fact | source |
|------|--------|
| 3 physical inputs: `up=GPIO1, down=GPIO2, power=GPIO0`, active-low | `simulator/board/murphy_m4.json:64-68`, `SimInput.h:10` + comment `m4_screen_viewer.py:36` "top→bottom: KEY1/Up(GPIO1), KEY2/Down(GPIO2), KEY_LOCK/Power(GPIO0)" |
| Touch FT6x36 on i2c0 `SDA13/SCL12/IRQ44/PWR45`, swapXY+flipY | `murphy_m4.json:71-88`, `HalGPIO.cpp:76-81` |
| Frontlight dual PWM `cool=GPIO48, warm=GPIO47` | `murphy_m4.json:89-99` |
| Power button is the **only** wake source (EXT1) on M4 deep sleep | `HalGPIO.cpp:154-157` `PowerManager::deepSleepUntilPowerButton()` + `main.cpp:439-476` `verifyPowerButtonDuration()` |
| Logical `BTN_BACK/CONFIRM/LEFT/RIGHT` indices exist but map through `SETTINGS.frontButton*` to touch footer slots, not left hardware | `CrossPointSettings.h:214-217` `frontButtonBack…`, `MappedInputManager.cpp:149-171` `mapButton()` + `228-229` `physical[4]` footer translation |
| `ButtonRemapActivity` reassigns the **bottom-bar order** on M4, not hardware front keys | `ButtonRemapActivity.cpp:26-41`, `176-178` `GUI.drawButtonHints(renderer, labelForHardware(BACK)…)` |
| X3-only IMU/knock sensors are absent on M4 profile | `murphy_m4.json:114-143` `"firmware_enabled": false, note: "NO_SENSORS"`, `SettingsLists.h:177` `#ifdef CROSSPOINT_X3` encloses tilt/tap/autoRotate |

Tick/knock (`tiltPageTurnEnabled`, `tapPageTurnEnabled`, `autoRotateEnabled`, `tiltScope` …) and `landscapeDualPage` via web-only `Reader` category do **not** appear in `SettingsHubPolicy.h:71-119` `kDisplayRows/kKeysRows/…`. The Hub correctly hides them on M4.

---

## 3. Method

1. Enumerated every device-visible key from `SettingsHubPolicy.h` (`kDisplayRows` 19, `kKeysRows` 6, `kNetworkRows` 6, `kSystemRows` 7) and cross-checked against `SettingsLists.h:16-325` and `SettingsActivity.cpp:70-147` `settingsHubContainsKey` bucketing.
2. For each key, located its read site: `CrossPointSettings.h:<field>`, `SETTINGS.*` consumers (reader, `main.cpp`, `MyLibraryActivity`, `EpdFontLoader`, `BluetoothHIDManager`, `GUI`), and its write site (`SettingsLists.h` or policy `m4Only` flag).
3. Tested the M4 input path: can `HalGPIO` ever produce it? Is touch already a superset?
4. Flagged `#ifdef CROSSPOINT_MURPHY_M4` / `X3` polarity mismatches.
5. No flash, no `git push origin`, no `pkill m4adb`, no `reset --hard`/`clean`.

---

## 4. Full inventory — every M4 Hub key

> Columns: `key` | hub/section | UI label (EN/CN) | verdict | evidence (file:symbol) | rationale vs 3-left-buttons + touch

### 4.1 DisplayReading (Hub 0) — 5 sections, 19 rows on M4, flattened 24 (5 headers + 19) per spec §4.1

| key | section | UI label | verdict | evidence | rationale (3-left + touch) |
|-----|---------|----------|---------|----------|----------------------------|
| `sleepScreen` | 0 界面 | 锁屏壁纸 `L(Str::kSleepScreen)` | **keep** | `SettingsLists.h:18` DynamicEnum → `CrossPointSettings.h:194` `sleepScreen` consumed in `SleepActivity` | Visual-only, no input conflict. All 6 wallpapers including `TRANSPARENT` overlay use framebuffer/pxc, independent of buttons/touch. |
| `statusBar` | 0 界面 | 阅读进度 `L(Str::kReadingProgressSetting)` | **keep** | `SettingsLists.h:47` Enum → `CrossPointSettings.h:200` `statusBar` read in reader header | Pure rendering; 6 modes (`无/不显示/完整+百分比…`) valid even with touch navigation. |
| `hideBatteryPercentage` | 0 界面 | 隐藏电池百分比 `L(Str::kHideBatteryPercent)` | **keep** | `SettingsLists.h:52` Enum → `CrossPointSettings.h:277` read in `FengyanTheme::drawBattery` / Scene header | Battery icon still shown via `$system.battery`; hiding percent is a display preference, touch-neutral. |
| `refreshFrequency` | 1 墨水屏刷新 | 刷新频率 `L(Str::kRefreshFrequency)` | **keep** | `SettingsLists.h:55` Value 1-30 → `CrossPointSettings.h:242` `getRefreshFrequency()` in reader cleanup cadence | E-ink cadence, input-neutral. |
| `neverFullRefresh` | 1 墨水屏刷新 | 永不全刷 `L(Str::kNeverFullRefresh)` | **keep** | `SettingsLists.h:56` Toggle → `CrossPointSettings.h:281` | Disables reader-body cleanup; meaningful on e-ink, not input-dependent. |
| `buttonHintsEnabled` | 0 界面 | 按钮提示 `L(Str::kButtonHints)` | **rewrite** | `SettingsLists.h:57` Toggle → `CrossPointSettings.h:326` gated in `LyraTheme.cpp:290` / `FengyanTheme.cpp:456` `if (!SETTINGS.buttonHintsEnabled) return;` + `SettingsHubPolicy.h:77` kDisplayRows | **Functional** but label assumes hardware button hints. On M4 there are **no front hardware buttons**; hints are the **bottom-bar 4 touch slots** (`LyraTheme::drawButtonHints` draws at `pageHeight - buttonHintsHeight` 4×106 px). When `0` (default off) the only discoverable affordance for Back/Confirm/Left/Right touch zones disappears, yet touch still works via `M4FooterTouchPolicy`. Should keep but **rename**: `底部触摸按键提示 (Bottom touch bar hints)` and default `On` on M4, or at minimum clarify scope. |
| `frontlightBrightness` | 2 前光 (m4Only) | 前光亮度 `L(Str::kFrontlightBrightness)` | **keep** | `SettingsLists.h:60` Value 0-100 → `CrossPointSettings.h:330` `frontlightBrightness` applied via `FrontlightManager` at boot/`MappedInputManager.cpp:60-68` side-swipe delta | Real dual-channel frontlight (`GPIO48/47`) on M4 `murphy_m4.json:89-99`. `m4Only` flag correctly hides on non-M4. |
| `frontlightWarmth` | 2 前光 (m4Only) | 前光色温 `L(Str::kFrontlightWarmth)` | **keep** | `SettingsLists.h:62` → `CrossPointSettings.h:331` same | Same as above; no input conflict. |
| `sleepBeforeFullRefresh` | 1 墨水屏刷新 | 关机前全刷 `L(Str::kFullRefreshBeforeSleep)` | **keep** | `SettingsLists.h:65` Toggle → `CrossPointSettings.h:340` consumed in `SleepActivity` | Visual-only. |
| `imageQuality` | 0 界面 | 图片质量 `L(Str::kImageQuality)` | **keep** | `SettingsLists.h:66` Enum `QUALITY_FAST/NORMAL/HD` → `CrossPointSettings.h:361` `EpdImageQuality` pipeline | No touch/hardware conflict. |
| `iconStyle` | 0 界面 | 图标风格 `L(Str::kIconStyle)` values `风格一/二/三` | **decide** | `SettingsLists.h:69` Enum → `CrossPointSettings.h:374` consumed **only** in `FengyanTheme.cpp:153` `iconForName()` for `*32` menu icons (`Folder32/History32/Netdisk32/Setting32/Wifi32/Shuqian32`). **Zero consumer** in `HomeSceneModel.cpp`, `GfxSceneRenderer`, `murphy_settings_*_m4theme` | On M4 the Home is **Scene-based** (`HomeSceneModel.cpp` has no `iconStyle` reference); Home app icons are **plugin BMPs** (`AppListActivity` + `HomeSceneAssetDecoder`). `iconStyle` only changes the old Fengyan 72×72 file-manager icons. Choices: (a) **hide on M4 Hub** (cleanest), (b) move to a file-manager submenu if that chrome is intentionally kept. Leaving it on the top-level Display hub suggests it still changes Home — which it no longer does on M4 — so it is at minimum confusing. Marked `decide` pending product on whether Fengyan file-manager theming is still user-facing on M4. |
| `homeIconStyle` | 0 界面 | 图标选中风格 `L(Str::kIconSelectedStyle)` values `仅四角/仅灰色背景…` | **hide** | `SettingsLists.h:72` Enum → `CrossPointSettings.h:371` **No consumer on M4**: grep `SETTINGS.homeIconStyle` / `homeIconStyle` across `firmware/src` returns **only persistence** (`CrossPointSettings.cpp:140,342,633,812`) + policy listing (`SettingsHubPolicy.h:82`). No read in `FengyanTheme`, `LyraTheme`, `HomeSceneModel`, `GfxSceneRenderer` | Fully dead on the current Scene Home — the spec itself flags this as a "Display leftover tied to old non-Scene home icon chrome". No renderer ever branches on it on M4. Persisting it wastes NVM and implies a visual change that never lands. **Should be gated off on M4** (CRITICAL for `hide`). |
| `uiFontSize` | 0 界面 (m4Only) | 系统字号 `L(Str::kUiFontSize)` 小/中/大 | **keep** | `SettingsLists.h:77` DynamicEnum → `CrossPointSettings.h:225` `getUiFontSize()/setUiFontSize()` + `SettingsActivity.cpp:486` `EpdFontLoader::applySystemChrome(renderer)` | Real system-chrome size via `EpdFontLoader`, independent of reader `readerPixelSize`. Correctly `m4Only`. |
| `readerLayout` | 3 阅读排版 | 阅读排版 `L(Str::kReaderLayout)` ACTION | **keep** | `SettingsActivity.cpp:96-99` ACTION → `EpubReaderSettingsActivity`; policy `kDisplayRows:85` entry `{"readerLayout",3}` non-m4Only | Spec §4.1 door to existing reader settings. No input conflict; opens reader typography/margins (still exposed via web `Reader` category). The spec explicitly says "不要把 Reader 目录键铺到这一页" — current single door is correct. |
| `systemAnimationEnabled` | 4 翻页动画 (m4Only) | 系统动画 `L(Str::kSystemAnimation)` | **keep** | `SettingsLists.h:242` Toggle → `CrossPointSettings.h:344` `systemAnimationEnabled` consumed in `HomeActivity` / `EpdAnimation` for non-reader transitions | M4-only flag correct (`#ifdef CROSSPOINT_MURPHY_M4`). Distinct from reader animation. Keep. |
| `pageTurnAnimationSteps` | 4 翻页动画 (m4Only) | 动画步数 `L(Str::kPageTurnAnimSteps)` 2-64 | **keep** | `SettingsLists.h:244` Value → `CrossPointSettings.h:350` consumed in `PageTurnAnimation` waveform LUT | M4-only strip animation, valid. |
| `pageTurnAnimationMult` | 4 翻页动画 (m4Only) | 窗口倍数 `L(Str::kPageTurnAnimMult)` 1-16 | **keep** | `SettingsLists.h:246` Value → `CrossPointSettings.h:352` | Same; literal LUT mult, keep. |
| `pageTurnAnimationTp` | 4 翻页动画 (m4Only) | 动画波形TP `L(Str::kPageTurnAnimTp)` 1-16 | **keep** | `SettingsLists.h:249` Value → `CrossPointSettings.h:354` | Same. |
| `pageTurnAnimationFrameRate` | 4 翻页动画 (m4Only) | 动画帧率 `L(Str::kPageTurnAnimFrameRate)` 慢/中/快 (LUT 0x22/0x44/0x88) | **keep** | `SettingsLists.h:252` DynamicEnum → `CrossPointSettings.h:356` consumed as LUT byte; spec warns against plain `Enum` here | Spec §4.1 keeps DynamicEnum/LUT mapping; correct on M4. Keep. |

### 4.2 KeysOperations (Hub 1) — single group, no section header, 6 rows per spec §4.2 — **all functional, 5 need rewrites**

| key | section | UI label (current) | verdict | evidence | rationale (3-left + touch) |
|-----|---------|--------------------|---------|----------|----------------------------|
| `remapButtons` | 0 (no header) | 重新映射前置按键 `L(Str::kRemapFrontButtons)` ACTION | **rewrite** | `SettingsHubPolicy.h:94` kKeysRows[0]; `SettingsActivity.cpp:101-105` ACTION → `ButtonRemapActivity`; `ButtonRemapActivity.cpp:26-100` 4 roles `Back/Confirm/Left/Right`, `HalGPIO.h:84-92` indices, `MappedInputManager.cpp:149-171` map via `frontButton*`, `M4FooterTouchPolicy` footer translation | **Live** — it reorders the bottom-bar touch slots on M4 (which is why `ButtonRemapActivity::render` previews them via `GUI.drawButtonHints(renderer, labelForHardware(BACK)…)`). But the wording "前置按键" (front hardware keys) is wrong on M4: M4 has **no 4 front physical keys**, only **3 left hardware keys + bottom-bar touch**. The side `Up(上)/Down(下)` used to reset/cancel (`ButtonRemapActivity.cpp:60-74`) are the *actual* left hardware, while the 4 re-mappable "front" keys are **touch**. Recommend: `底部触摸按键映射 (Bottom touch shortcuts)` or `触摸快捷键布局`. Also note `getPressedFrontButton()` reads `HalGPIO::BTN_*` edges which on M4 only fire via `M4FooterTouchPolicy::slotFromPoint` → `wasReleased` footer synthesis (so `wasPressed` path has a gap — footer taps surface as `wasReleased`, not `wasPressed`; see `MappedInputManager.cpp:177-235`). The UI still works because `remapButtons` waits for `getPressedFrontButton` via `wasPressed(BTN_*)`; touch footer taps may need an extra tap to register — product should verify on device that footer → `wasPressed` path is wired for remap, or change it to listen on `wasReleased`. |
| `sideButtonLayout` | 0 | 侧边按钮设置（仅阅读） `L(Str::kSideButtonSettings)` options `上, 下 / 下, 上` | **rewrite** | `SettingsLists.h:167` Enum → `CrossPointSettings.h:83` `SIDE_BUTTON_LAYOUT`; `MappedInputManager.cpp:24-28` `kSideLayouts[] = {{UP,DOWN},{DOWN,UP}}`; `MappedInputManager.cpp:168-171` `PageBack/PageForward` routing; `SettingsHubPolicy.h:95` | **Live and correct** (`PREV_NEXT` vs `NEXT_PREV` swaps which of the left `Up/Down` means Prev/Next in readers via `PageBack/PageForward`). Wording "侧边按钮" is historically "side" (X4 side keys) but on M4 it's **左侧** left edge (the spec brief already mislabels it as "side" in one code comment). And scope note "（仅阅读）" is accurate — only `EpubReaderActivity` / `TxtReaderActivity` branch on `PageBack/PageForward` (plus BT remap) — but the Hub label should make that explicit: **suggest `左侧翻页键布局 (仅阅读)`** and options `上=上一页 / 下=下一页` rather than bare "上,下". Keeping the key, rewriting label/options, is cleaner than hiding. |
| `shortPwrBtn` | 0 | 短按电源键 `L(Str::kShortPowerButton)` values `忽略/休眠/翻页/全刷/确认` | **rewrite** | `SettingsLists.h:171` Enum `SHORT_PWRBTN` → `CrossPointSettings.h:182,205` ; `main.cpp:1490-1506` maps `FULL_REFRESH` → `FAST_REFRESH` (reader skips), `CONFIRM` → `gpio.injectButtonPress(BTN_CONFIRM)`; `EpubReaderActivity.cpp:854-870` `powerPageTurn` merges power into next-page when `== PAGE_TURN` (affected by `longPressChapterSkip` `usePressForPageTurn`); `CrossPointSettings.h:427` `getPowerButtonDuration()` still `10 ms` when `== SLEEP` else `400 ms` | **Live**. Power is the **bottom of the 3 left keys** and the *only wake source* (`HalGPIO.cpp:154`). Five values are all wired, but the set is confusing against 3-btn+touch: `翻页` makes Power an extra page-turn key that duplicates `Up/Down` + swipe + tap zones; `确认` makes Power inject a `Confirm` tap that duplicates the central bottom-bar Confirm slot and the `Confirm` hardware-touch mapping; `全刷` is legacy (now downgraded to `FAST_REFRESH`, with reader explicitly skipping it at `EpubReaderActivity.cpp:858`). Best rewrite rather than hide: keep the 5 values but **clarify Power identity** in labels — e.g. `短按电源键 (左侧底部键)` and rename options to `忽略 / 休眠(含10ms快醒) / 作为翻页键 / 刷新屏幕 / 作为确认键`. Product question: does `休眠` (which shortens the long-press-to-sleep threshold from 400 ms to 10 ms, `CrossPointSettings.h:427`) conflict with `longPressBoot` (2 s hold to boot)? No — one is runtime, one is `verifyPowerButtonDuration()` only on wake-from-deep-sleep; but the UI should hint the 10 ms consequence. |
| `longPressChapterSkip` | 0 | 长按左右键跳章节 `L(Str::kLongPressChapterSkip)` | **keep** (rewrite optional) | `SettingsLists.h:169` Toggle → `CrossPointSettings.h:284` ; `EpubReaderActivity.cpp:797` `usePressForPageTurn = !longPressChapterSkip`, `829-845` `isPhysicalButton && held>1000ms` → `currentSpineIndex ±1`, `894-932` BT hold `BT_HOLD_THRESHOLD=3, BT_IDLE_RESET_MS=950, BT_MIN_HOLD_MS=2000` | **Live** — gates the long-hold chapter-skip on physical `PageBack(Up)/PageForward(Down)/Left/Right` (>1 s) and on BT virtual repeats. Label "左右键" is ambiguous on M4: it actually means **左侧 Up/Down page keys** (and Right/Left touch Confirm-side moves in readers). On M4 the Up key is the top left key, not "left". Keeping the feature, optionally rewriting label to `长按翻页键跳章节` to avoid implying front-left/right hardware. |
| `longPressBoot` | 0 | 长按开机 `L(Str::kLongPressBoot)` | **keep** (rewrite optional) | `SettingsLists.h:174` Toggle → `CrossPointSettings.h:279` ; `main.cpp:439-476` `verifyPowerButtonDuration()` checks `if (!SETTINGS.longPressBoot) return;` else requires 2 s `BTN_POWER` hold + post-wake re-arm before `deepSleepUntilPowerButton()` | **Live and important**. M4's factory sleep is deep-sleep with rail hold (`HalGPIO.cpp:156`); without this gate a pocket brush of Power would wake. Label `长按开机` understates: it gates **wake from deep sleep**, independent of `shortPwrBtn` (`main.cpp:437` comment). Optional rewrite: `唤醒需长按电源键2秒`. Keep value. |
| `libraryLongPressMenu` | 0 | 长按确认键打开菜单 `L(Str::kLongPressConfirmMenu)` | **rewrite** | `SettingsLists.h:175` Toggle → `CrossPointSettings.h:387` ; `MyLibraryActivity.cpp:768-894` : `if (SETTINGS.libraryLongPressMenu && isPressed(Confirm) && held>=700)` → `showingActionMenu=true` else `wasReleased(Confirm)` toggles between menu vs direct open; `SettingsHubPolicy.h:99` | **Live** — toggles `Confirm` semantics inside **文件管理** only (scope-limited). But label `长按确认键打开菜单` on M4 means **bottom-bar Confirm slot long-press**, not a hardware key. And it lacks scope hint. Default `1` (long-press) is sensible with touch (short-press = open file). Keep but **rewrite** to include scope: `文件管理: 长按确认键打开菜单` (or with value texts `单击打开 / 长按打开菜单` vs `单击打开菜单 / 长按打开`). Evidence the feature only fires in `MyLibraryActivity`, not catalogs. |

### 4.3 NetworkSync (Hub 2) — 2 sections (2+4), 6 rows, no M4 gating needed

All six are **keep** — none depend on left-button geometry; `bluetooth` is not a stub.

| key | section | UI label | verdict | evidence | rationale |
|-----|---------|----------|---------|----------|-----------|
| `wifiAlwaysReselect` | 0 网络 | 每次都重新选择WIFI `L(Str::kWifiAlwaysReselect)` | **keep** | `SettingsLists.h:231` Toggle → `CrossPointSettings.h:334`; consumed in `WifiSelectionActivity` | WiFi strategy, no touch/hardware dependency. |
| `autoSyncTimeOnBoot` | 0 网络 | 开机自动同步时间 `L(Str::kAutoSyncTimeOnBoot)` | **keep** | `SettingsLists.h:230` → `CrossPointSettings.h:390` ; main NTP sync | Touch-neutral. |
| `bluetooth` | 1 同步入口 | 蓝牙设置 `L(Str::kBluetoothSettings)` ACTION | **keep** | `SettingsHubPolicy.h:105` ; `SettingsActivity.cpp:107-109` ACTION → `SimpleBluetoothActivity`; `BluetoothHIDManager.h:41-100` full NimBLE host, not stub; `CrossPointSettings.h:272` `bluetoothEnabled` | Real BLE HID scan/connect/key-learn (`SimpleBluetoothActivity.cpp:40-`). The spec concern "bluetooth stubs" does not apply — this is a full HID host. Keep. |
| `koreader` | 1 | KOReader 同步 ACTION | **keep** | `SettingsHubPolicy.h:106` ; persists via `KOReaderCredentialStore` (`SettingsLists.h:273-302`) | Web sync credentials, touch-neutral. |
| `jianguo` | 1 | 坚果云配置 ACTION | **keep** | `SettingsHubPolicy.h:107` ; `SettingsLists.h:313-318` `JG*` char arrays | Same. |
| `dataCapsule` | 1 | 数据胶囊配置 ACTION | **keep** | `SettingsHubPolicy.h:108` ; `SettingsLists.h:321-324` DataCapsule WebDAV | Same. |

### 4.4 SystemMaintenance (Hub 3) — 2 sections (3+4), 7 rows on M4, 5 without `m4Only`

| key | section | UI label | verdict | evidence | rationale |
|-----|---------|----------|---------|----------|-----------|
| `systemLanguage` | 0 系统 | 系统语言 `L(Str::kSystemLanguage)` values 简体/繁体 | **keep** | `SettingsLists.h:228` Enum → `CrossPointSettings.h:409` ; `I18n.h:554` `SETTINGS.systemLanguage` switches `STRINGS_ZH_*` | Works; no touch conflict. |
| `sleepTimeout` | 0 系统 | 休眠时间 `L(Str::kSleepTimeout)` 1/5/10/15/30 min | **keep** | `SettingsLists.h:233` Enum → `CrossPointSettings.h:239` ; `main.cpp:1481` `getSleepTimeoutMs()` | Auto-sleep timer, device-wide, Keep. |
| `directTxtRead` | 0 系统 | 直读TXT文档 `L(Str::kDirectTxtRead)` | **keep** | `SettingsLists.h:232` Toggle → `CrossPointSettings.h:406` ; `RecentBooks` txt path handling | Choose txt vs epub conversion; no input relevance. |
| `clearCache` | 1 维护 | 清理缓存 ACTION | **keep** | `SettingsHubPolicy.h:114` ; `SettingsActivity.cpp:127` → `ClearCacheActivity` | Keep. |
| `resetSettings` | 1 维护 | 还原为初始设置 ACTION | **keep** | `SettingsHubPolicy.h:115` ; `SettingsActivity.cpp:132` → `ResetSettingsActivity` | Keep. |
| `developerOptions` | 1 维护 (m4Only) | 开发者选项 `L(Str::kDeveloperOptions)` ACTION | **keep** | `SettingsHubPolicy.h:117` `m4Only:true` ; `SettingsActivity.cpp:137` `#ifdef CROSSPOINT_MURPHY_M4` → `DeveloperOptionsActivity` (USB serial bridge gating `developerSerialDebugEnabled`) ; `CrossPointSettings.h:414` | Real M4-only gate; Bridge authorization `main.cpp:1543` `gM4DebugBridge.setAuthorized(DEVELOPER)`. Correct. |
| `switchBootSlot` | 1 维护 (m4Only) | 切换启动区 `L(Str::kSwitchBootSlot)` ACTION `APP0/APP1` | **keep** | `SettingsHubPolicy.h:118` ; `SettingsActivity.cpp:142-144` `#ifdef` → `esp_ota_set_boot_partition` in `runningOta*` `main.cpp:456` partition map | Real M4 OTA slot; legit. |

---

## 5. Non-Hub settings (web/API-only `Reader` category) — correctly hidden on device

The `Reader` category in `SettingsLists.h` (lines 87-164: `firstlineintented`, `readerPixelSize`, `lineSpacing`, `wordSpacing`, `screenMargin_*`, `ReadingScreenEnabled`, `extraline`, `underlineOffset`, `underlineStyle`, `extraParagraphSpacing`, `paragraphAlignment`, `showTimeInsteadOfChapter`, `showEpubImages`, `punctWidth`, `landscapeDualPageEnabled`, `pageTurnAnimationEnabled/Dir`) is **not** enumerated in `SettingsHubPolicy.h:71-119` and is intentionally absent from `SettingsActivity.cpp:82-92` bucket routing. The spec §4.1 explicitly requires this ("不要把 Reader 目录键铺到这一页" — only the `readerLayout` door belongs on the Hub). So on M4:

- `landscapeDualPageEnabled` (`SettingsLists.h:156` `Toggle` → `CrossPointSettings.h:323` → `EpubReaderActivity.cpp:50` `orientation==LANDSCAPE_CCW` gate) is **not a Hub row**; its runtime is only armed when `orientation` becomes `LANDSCAPE_CCW` via the 1 s hold of `PageBack(Up)` (`EpubReaderActivity.cpp:542-572`). Keeping it web-only is correct; exposing it on M4 Hub would imply a separate landscape toggle while the actual entry is the hold gesture — that would duplicate state.
- Tilt/tap/autoRotate (`SettingsLists.h:177-224` inside `#ifdef CROSSPOINT_X3`) and `autoPageTurnEnabled` etc are **already invisible on M4** via `#ifdef` — correct polarity.
- No wrong-`#ifdef` item was found in the Hub policy: every `m4Only` flag matches its `SettingsLists.h` guard (`frontlight*`, `uiFontSize`, `systemAnimation*`, `pageTurnAnimation*`, `developerOptions`, `switchBootSlot`). The non-M4 rows (`iconStyle`, `homeIconStyle`, `buttonHintsEnabled`, …) deliberately have no gate and correctly appear on both SKUs (though `homeIconStyle` should still be M4-hidden for the Scene reason above).

Therefore **no extra hide** is needed for the web-only Reader fine settings — the current omission is by design.

---

## 6. Focus section answers (brief-mandated keys)

| prompt key | verdict & one-liner |
|------------|---------------------|
| `sideButtonLayout` | **rewrite** — live (swaps left `Up↔Down` Prev/Next via `MappedInputManager::kSideLayouts`), label misuses "侧边" on a left-only device; see §4.2 |
| `shortPwrBtn` | **rewrite** — 5 actions live via `main.cpp:1490` + `EpubReaderActivity.cpp:854`, but `翻页` duplicates side keys+swipe and `全刷` is legacy `FAST_REFRESH`; label should state it's the bottom left physical key |
| `longPressBoot` | **keep** — real 2 s wake gate `main.cpp:439` independent of `shortPwrBtn`; rename to wake semantics optionally |
| `remapButtons` | **rewrite** — reorders bottom touch footer, not front hardware; `ButtonRemapActivity` wording "前置按键" is wrong for M4 |
| `buttonHintsEnabled` | **rewrite** — gates bottom-bar drawing `LyraTheme.cpp:290`; default off hides the only discoverable affordance for the 4 touch zones |
| `longPressChapterSkip` | **keep** — physical & BT hold-skip (`EpubReaderActivity.cpp:829,894`), label "左右键" slightly ambiguous on M4 |
| `libraryLongPressMenu` | **rewrite** — file-manager `Confirm` semantics (`MyLibraryActivity.cpp:768`), needs scope in label |
| `iconStyle` | **decide** — only file-manager 32 icons (`FengyanTheme.cpp:153`), not Home Scene; M4 Home ignores it |
| `homeIconStyle` | **hide** — fully dead on M4, zero consumer outside persistence |
| `landscapeDualPage` | **keep hidden** — correctly not on M4 Hub (web-only Reader key); runtime needs `LANDSCAPE_CCW` orientation which is entered via hold-`PageBack` |
| `bluetooth` | **keep** — real HID host, not stub (`BluetoothHIDManager.h`) |
| wrong-`#ifdef` | **none found** in Hub policy — all `m4Only` flags match `SettingsLists.h` guards |

---

## 7. Proposed hide list (M4-only gate)

> If product confirms the `iconStyle`/`homeIconStyle` analysis, the minimal safe diff is a host-enforced Hub filter — no persistence deletion (settings stay readable via JSON/API, just not drawn as Hub rows).

```cpp
// In SettingsHubPolicy.h — the 2-row, 4-column hit-tested change.
// Do NOT delete the CrossPointSettings fields; gate only the M4 Hub.

inline constexpr CatalogRow kDisplayRows[] = {
    // …
    {"imageQuality", 0}, false,
-   {{"iconStyle", 0}, false},
-   {{"homeIconStyle", 0}, false},
+   {{"iconStyle", 0}, false},          // M4: keep only if file-manager theming stays Hub-visible
+   {{"homeIconStyle", 0}, false},      // M4: hide (Scene Home ignores it) — set m4Only=true
    {{"uiFontSize", 0}, true},
    // …
};

// Minimal correct gating for M4:
  {{"iconStyle", 0}, /*m4Only=*/ false},      // keep — or set true if hub clutter outweighs file-icon use
  {{"homeIconStyle", 0}, /*m4Only=*/ true},   // hide on M4 — dead on Scene Home
```

Concrete proposal for M4 hub (2 keys, no migration):

```
hide_on_m4 = [
  "homeIconStyle",   // CRITICAL — proven dead, zero consumer on M4
]

decide_on_m4 = [
  "iconStyle",       // only file-manager icons; decide per product: hide on M4 Hub or leave as "文件列表图标风格"
]
```

There is **no other proven-dead M4 Hub row** warranting hide. All other 36 rows have a live consumer or a correct `m4Only` gate.

### Host lock test that would enforce it

```
test_settings_m4_hide_gate.cpp (native, no device)

#include "activities/settings/SettingsHubPolicy.h"
TEST(M4HideGate, homeIconStyle_hidden_on_M4) {
  for (int i=0; i<settingsHubRowCount(DisplayReading,true,false); ++i)
    EXPECT_STRNE(settingsHubRowAt(DisplayReading,i,true,false).key, "homeIconStyle");
  // Optional IconStyle decision — pick one:
  // EXPECT_FALSE(settingsHubContainsKey(DisplayReading,"iconStyle",true,false)); // if hidden
  // EXPECT_TRUE(settingsHubContainsKey(DisplayReading,"iconStyle",true,false)); // if kept
}
```

The 38→36/37 hub-row count propagates to flattened: DisplayReading flattened drops 24 → 23 (with one hide) or 22 (with two).

---

## 8. Proposed rewrite list (old label → suggested, plus option renames)

> None of these change keys or persistence — purely i18n/display strings in `I18n.h:STR_TABLE` and the `enumValues` arrays in `SettingsLists.h`.

| key | old `L(Str::…)` (CN) | suggested (CN) | scope note |
|-----|----------------------|---------------|------------|
| `buttonHintsEnabled` | 按钮提示 `kButtonHints` | **底部触摸按键提示** | Clarifies it controls the 4-slot bottom bar `LyraTheme::drawButtonHints` (106 px each, portrait-locked), not the 3 left hardware keys. Consider default `On` on M4. |
| `remapButtons` | 重新映射前置按键 `kRemapFrontButtons` | **底部触摸按键映射** / 触摸快捷键布局 | M4 has no front hardware; `ButtonRemapActivity` already previews via `drawButtonHints`. |
| `sideButtonLayout` | 侧边按钮设置（仅阅读） `kSideButtonSettings` / options `上, 下` `下, 上` | **左侧翻页键布局 (仅阅读)** / options `上=上一页 / 下=下一页` | `MappedInputManager.cpp:24` actually maps left physical `Up/Down`; "侧边" is X4 heritage. |
| `shortPwrBtn` | 短按电源键 `kShortPowerButton` / `忽略/休眠/翻页/全刷/确认` | **短按电源键 (左侧底部键)** / options `忽略 / 休眠(10ms快醒) / 作为翻页键 / 刷新屏幕 / 作为确认键` | Power is the bottom of the 3 left keys and the only wake source. `休眠` shortening threshold (10ms vs 400ms) deserves parenthetical. `全刷` is now `FAST_REFRESH`. |
| `libraryLongPressMenu` | 长按确认键打开菜单 `kLongPressConfirmMenu` | **文件管理: 长按确认键的操作** (values `单击打开菜单` vs `长按打开菜单`) | Scope is `MyLibraryActivity` only, not global Confirm. |
| `longPressChapterSkip` | 长按左右键跳章节 `kLongPressChapterSkip` | **长按翻页键跳章节** (or `长按翻页键(1秒)跳章节`) | On M4 "左右" misleads; it's `Up/Down` page keys (and BT). |
| `longPressBoot` | 长按开机 `kLongPressBoot` | **唤醒需长按电源键2秒** | Actual semantics is wake-from-deep-sleep (`verifyPowerButtonDuration`), not cold boot. |
| `iconStyle` (if kept) | 图标风格 `kIconStyle` | **文件列表图标风格** | Narrows scope to file-manager 32 icons (the only live consumer on M4). |

If only the four strongest are taken, pick: `remapButtons`, `sideButtonLayout`, `shortPwrBtn`, `libraryLongPressMenu` — these are the ones where the old wording literally names the wrong physical button.

---

## 9. Open product questions (Needs product decision)

1. **Home vs file-manager icon styling** — `iconStyle` currently paints only Fengyan file-manager 32 px icons (`FengyanTheme.cpp:153`). If M4 is Scene-only and the file manager uses a single built-in 24 px set, should `iconStyle` stay on the M4 Hub at all, or move under a file-manager advanced screen? If hidden, confirm the 32-icon theming is not a shipping feature on M4. **Recommendation: hide `homeIconStyle`, decide `iconStyle` per this answer.**

2. **`buttonHintsEnabled` default on M4** — Today default `0` (off) (`CrossPointSettings.h:326`), so a new M4 hides the bottom-bar affordance for `Back/Confirm/Left/Right` touch zones (`MappedInputManager.cpp:219`). With no physical front keys, first-run discoverability suffers. **Should M4 default flip to `1` (on)?** This is a product retention question, not a correctness fix — hiding without a default change ships an undiscoverable UI.

3. **`shortPwrBtn == CONFIRM` injection semantics** — `main.cpp:1502` injects `BTN_CONFIRM` as a virtual press on Power release. That fires `wasReleased(BTN_CONFIRM)` in the current activity in the *next* loop frame. In `SettingsActivity.cpp:291` and readers this competes with `longPressChapterSkip`'s `usePressForPageTurn` toggle and with `Power`-as-page-turn (109 vs 854). Is injecting Confirm from Power intended to work inside the file manager (where `libraryLongPressMenu` also keys on Confirm)? Need product confirmation of the intended Power→Confirm hand-off, especially since long-press Power (>400 ms) already triggers deep sleep (`main.cpp:1509`).

4. **`ButtonRemapActivity` footer synthesis on M4** — `MappedInputManager::wasPressed` does not synthesize footer taps (only `wasReleased` does at `MappedInputManager.cpp:219`). `ButtonRemapActivity::loop` waits on `getPressedFrontButton()` → `wasPressed(BTN_*)` (`ButtonRemapActivity.cpp:84`), so a bottom-bar tap remap assignment may require two taps or a real `BTN_*` edge. **Should remap listen on `wasReleased` / handle footer taps explicitly for M4?** Current report marks it as a rewrite (wording) but the touch → pressed path may also need a code follow-up if repro'd on device.

5. **`landscapeDualPage` on-device visibility** — It's web-only by design per spec §4.1. But `EpubReaderActivity.cpp:542` lets the user enter landscape via an undiscoverable hold of the top left key. Should `landscapeDualPage` become a second toggle in `EpubReaderSettingsActivity` (reader door), or stay invisible and default `0`? Keeping it hidden is fine if landscape dual-page is not a M4 reader feature.

6. **`autoSyncTimeOnBoot` / `wifiAlwaysReselect` copy** — Labels are verbose translations (`每次都重新选择WIFI` has mixed caps). Minimal rename is not compatibility, just polish — include if a rewrite pass is already touching `I18n.h`.

---

## 10. Evidence appendix — file:symbol index (every hide/rewrite traced)

| key | primary read site | secondary consumer / gate |
|-----|-------------------|---------------------------|
| `buttonHintsEnabled` | `CrossPointSettings.h:326` `buttonHintsEnabled` | `SettingsLists.h:57` Toggle; `SettingsHubPolicy.h:77` ; `LyraTheme.cpp:290` `drawButtonHints(…)` gating + `FengyanTheme.cpp:456` same; `SettingsActivity.cpp:748` `mapLabels(…)` |
| `frontlightBrightness` / `Warmth` | `CrossPointSettings.h:330-331` | `SettingsLists.h:60,62` `#ifdef CROSSPOINT_MURPHY_M4`; `SettingsHubPolicy.h:78-79` `m4Only:true`; `MappedInputManager.cpp:60` side-swipe applies |
| `iconStyle` | `CrossPointSettings.h:374` | `SettingsLists.h:69` Enum; `SettingsHubPolicy.h:82` ; **consumer** `FengyanTheme.cpp:153` `SETTINGS.iconStyle` in `iconForName()` for `*32` icons; **zero** consumer in `HomeSceneModel.cpp` / `GfxSceneRenderer` |
| `homeIconStyle` | `CrossPointSettings.h:371` | `SettingsLists.h:72` Enum; `SettingsHubPolicy.h:83` ; **consumers: none** — only `CrossPointSettings.cpp:140,342,633,812` persistence; proving *dead* |
| `uiFontSize` | `CrossPointSettings.h:225` `getUiFontSize()` | `SettingsLists.h:77` `#ifdef CROSSPOINT_MURPHY_M4`; `SettingsHubPolicy.h:84` `m4Only:true`; `SettingsActivity.cpp:486` `EpdFontLoader::applySystemChrome` |
| `remapButtons` | `SettingsHubPolicy.h:94` kKeysRows[0] | `SettingsActivity.cpp:101` ACTION → `ButtonRemapActivity`; `ButtonRemapActivity.cpp:26` roles, `M4FooterTouchPolicy` footer preview |
| `sideButtonLayout` | `CrossPointSettings.h:83,211` `SIDE_BUTTON_LAYOUT` | `SettingsLists.h:167` ; `SettingsHubPolicy.h:95` ; `MappedInputManager.cpp:24-28` `kSideLayouts`, `149-171` PageBack/Forward |
| `shortPwrBtn` | `CrossPointSettings.h:182,205,427` | `SettingsLists.h:171` `SHORT_PWRBTN`; `main.cpp:1490` and `EpubReaderActivity.cpp:854` plus `getPowerButtonDuration()` |
| `longPressChapterSkip` | `CrossPointSettings.h:284` | `SettingsLists.h:169` ; `EpubReaderActivity.cpp:797,829,894` physical+BT chapter skip |
| `longPressBoot` | `CrossPointSettings.h:279` | `SettingsLists.h:174` ; `main.cpp:439` `verifyPowerButtonDuration()` |
| `libraryLongPressMenu` | `CrossPointSettings.h:387` | `SettingsLists.h:175` ; `MyLibraryActivity.cpp:768` file-manager branching |
| `bluetooth` | `CrossPointSettings.h:272` `bluetoothEnabled` | `SettingsHubPolicy.h:105` ; `SimpleBluetoothActivity.cpp:1` + `BluetoothHIDManager.h:41` real host |
| `landscapeDualPageEnabled` | `CrossPointSettings.h:323` | `SettingsLists.h:156` (Reader, web-only, **not** in Hub policy) ; `EpubReaderActivity.cpp:50` gate with `LANDSCAPE_CCW` + hold gesture at `542` |
| Left-button pins | — | `murphy_m4.json:64-68` `up:1 down:2 lock_power:0`, `SimInput.h:10` `MurphyButton::{Up,Down,Power}` |
| Touch FT6x36 | — | `murphy_m4.json:71-88` + `HalGPIO.cpp:76` `InputManager::hasTouch()` |
| X3 sensor absence | — | `murphy_m4.json:132-143` `NO_SENSORS` + `SettingsLists.h:177` `CROSSPOINT_X3` |

---

## 11. Risks & next steps

- **No code change is attached** to this report — per task "report-only unless crystal-clear and diff small". The only crystal-clear hide is `homeIconStyle` (dead code, no migration cost). The `iconStyle` decision is intentionally left as `decide` until file-manager theming is confirmed.
- If product picks the hide list (§7), the Hub diff is 4 lines + a 15-line host test; the flattened counts (Display 24→23 or 22) propagate only to `SettingsHubPolicy.h`; no SettingsActivity logic changes beyond `settingsFlatCount/syncWindow` (which already re-derives windows).
- No `platformio.ini` flag, QEMU fixturing, or `M4_AGENT_LESSONS.md` change is needed unless product asks to default `buttonHintsEnabled=1` on M4.
- No device screenshot is material — all findings are source-proved. A spot QEMU run with `murphy_m4_qemu` profile would confirm the Hub row count drops 19→18 for DisplayReading after the `homeIconStyle` gate, but is not required to land the report.

---

## 12. Definition of done (per round-8 brief)

- [x] Report committed at `docs/orchestration/rounds/round-8-muse-settings-device-compat-REPORT.md` with complete tables for all suspect and all Controls/Keys keys.
- [x] Every `hide`/`rewrite` row cites `file:symbol`.
- [ ] Host lock test — deferred until product confirms hide list (diff stays small and gated behind M4).
- [ ] Commit `round-8(muse): audit M4 settings vs 3-btn+touch` (next step — not pushed to origin).

---

*Forbidden actions not taken: `git push origin`, hardware flash, `git reset --hard`, `git clean`, `pkill -f m4adb`, Scene/Home theme rewrite. This report is English as requested for ROUND 8 Ultra.*
