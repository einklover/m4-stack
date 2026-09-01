# Round 8b — Muse: CORRECTED settings audit — M4 has NO bottom 4-slot footer

Date: 2026-09-01
Workspace: `/Volumes/z/paseo/workspaces/paseo/worktrees/041rfr5o/m4-home-muse-impl`
Auditor: Muse Ultra (`muse-spark-1.2`)
Baseline: `docs/orchestration/rounds/round-8-muse-settings-device-compat-REPORT.md` (`693bcc1`)
Brief: `docs/orchestration/rounds/round-8b-muse-settings-no-footer-slots.md`
Mode: **REPORT ONLY** — no code changes, no flash, no push.

---

## 1. Device model correction (authoritative)

> **Murphy M4 does NOT have a bottom-bar four-slot touch chrome (no Back/Confirm/Left/Right footer zones).**

Real M4 inputs (product truth, source-proved):

1. **Three physical buttons on the LEFT** — Up / Down / Power, top→bottom.
   `BoardConfig.h:845` `MURPHY_M4` `InputPins { -1, -1, -1, -1, 1, 2, 0, false }` — `back=UNASSIGNED, confirm=UNASSIGNED, left=UNASSIGNED, right=UNASSIGNED, up=GPIO1, down=GPIO2, power=GPIO0 (active-low)`, `InputStyle::DigitalButtons`.
   `InputManager.cpp:235` `isDigitalPressed(pin)` returns `false` when `pin < 0`; therefore `BTN_BACK=0 / CONFIRM=1 / LEFT=2 / RIGHT=3` **never fire physically on M4** — only `BTN_UP=4, DOWN=5, POWER=6` are live.
2. **Full-screen capacitive touchscreen** — direct taps/swipes on content (lists, cards, scenes, popups), not a dedicated footer strip.
   `BoardConfig.h:848` `TouchConfig { Ft6x36, SDA13, SCL12, IRQ44, powerEnable 45, swapXY+flipY }`, `Murphy M4.json:71-88` + `HalGPIO.cpp:76` `hasTouch()`.

Implication: any code that **draws, hit-tests, or remaps a bottom strip of four 106-px buttons** is **legacy / incompatible chrome** for M4, not the shipping UX. Do **not** rewrite labels to `底部触摸按键`; that doubles down on a surface the device does not ship.

This report reclassifies every Round-8 row that assumed the footer and lists the footer-painted debt.

---

## 2. What changes versus Round 8

Round 8 treated `M4FooterTouchPolicy` + `GUI.drawButtonHints` as the shipping M4 affordance for its four logical buttons `Back/Confirm/Left/Right` and proposed rewriting those Settings rows toward `底部触摸按键`. That direction is invalidated by the correction.

| key | Round-8 verdict | Corrected verdict | Why |
|-----|-----------------|-------------------|-----|
| `buttonHintsEnabled` | **rewrite** → `底部触摸按键提示` | **hide** | Gates `LyraTheme/FengyanTheme::drawButtonHints` (4 × 106 px footer). That footer *is* the claimed-non-existent chrome. No M4 shipping surface to hint. See §5. |
| `remapButtons` | **rewrite** → `底部触摸按键映射` | **hide** (on M4) | Reassigns the four `FRONT_HW_*` logical roles and drives `M4FooterTouchPolicy::slotFromPoint` + `ButtonRemapActivity` footer preview. Remaps a surface that does not exist. Backed by `BoardConfig` proof that front roles have no pins. |
| `libraryLongPressMenu` | **rewrite** scoped to Confirm slot | **hide** | `Confirm = BTN_CONFIRM=1` never fires physically on M4 (`back/confirm` pins UNASSIGNED). Without the footer, its only triggers are optional `shortPwrBtn==CONFIRM` inject or BT HID. Row tap already opens files via `wasScreenTapped` → `openSelected()`, not via Confirm. No provable real touch gesture for `Confirm` without footer chrome. |
| `sideButtonLayout` | rewrite left page keys | **keep** (rewrite wording only) | Live: swaps left `Up↔Down` as `PageBack/PageForward` via `MappedInputManager::kSideLayouts`. No footer dependency. |
| `shortPwrBtn` | rewrite | **keep** (rewrite wording only, LEFT-PHYSICAL) | Bottom of the 3 left keys; only wake source. All five values wired, no footer wording. |
| `longPressBoot` | keep | **keep** | Wake-from-deep-sleep gate (`verifyPowerButtonDuration`), left Power. |
| `longPressChapterSkip` | keep | **keep** | Long-hold on left `Up/Down` page keys (+ BT). |
| `homeIconStyle` | hide | **hide** (unchanged) | |
| `iconStyle` | decide | **decide** (unchanged) | |

Counts under corrected model (38 device-Hub rows, M4 build):

| hub | keep | rewrite (wording-only, left-physical) | hide | decide |
|-----|------|----------------------------------------|------|--------|
| DisplayReading (19) | 14 | 0* | 3 (`buttonHintsEnabled` + `homeIconStyle` + `iconStyle` if hidden) | 1 (`iconStyle` if kept) |
| KeysOperations (6) | 3 (`sideButtonLayout`, `shortPwrBtn`, `longPressBoot`/`longPressChapterSkip` verbatim — see §3.2) | 0 new footer rewrites; left-physical wording is optional polish | 2 (`remapButtons`, `libraryLongPressMenu`) | 0 |
| NetworkSync (6) | 6 | 0 | 0 | 0 |
| SystemMaintenance (7) | 7 | 0 | 0 | 0 |
| **total** | **30** | **0-3 wording-only** | **5-6** | **0-1** |

* `buttonHintsEnabled` moves from rewrite to hide; Display rewrites drop to zero unless `iconStyle` is kept with narrowed scope.

Hide-on-M4 grows from 1 (`homeIconStyle`) to **4–5** proven rows (see §4).

---

## 3. Revised inventory

### 3.1 DisplayReading (Hub 0) — 19 rows on M4

| key | section | UI label | corrected verdict | evidence | rationale vs LEFT 3 + full-screen touch |
|-----|---------|----------|-------------------|----------|-----------------------------------------|
| `sleepScreen` | 0 界面 | 锁屏壁纸 | **keep** | `SettingsLists.h:18` → `CrossPointSettings.h:194` | Wallpapers, no input. |
| `statusBar` | 0 界面 | 阅读进度 | **keep** | `SettingsLists.h:47` → `CrossPointSettings.h:200` | Header progress, touch-neutral. |
| `hideBatteryPercentage` | 0 界面 | 隐藏电池百分比 | **keep** | `SettingsLists.h:52` → `CrossPointSettings.h:277` | Icon rendering, no button. |
| `refreshFrequency` | 1 墨水屏刷新 | 刷新频率 | **keep** | `SettingsLists.h:55` → `CrossPointSettings.h:242` | E-ink cadence. |
| `neverFullRefresh` | 1 墨水屏刷新 | 永不全刷 | **keep** | `SettingsLists.h:56` → `CrossPointSettings.h:281` | Cleanup toggle. |
| `buttonHintsEnabled` | 0 界面 | 按钮提示 | **hide (on M4)** | `SettingsLists.h:57` → `CrossPointSettings.h:326` gated in `LyraTheme.cpp:290` / `FengyanTheme.cpp:456` `if (!buttonHintsEnabled) return;` ; `SettingsHubPolicy.h:77` | **Corrected**: gated the 4-slot footer `FengyanTheme/LyraTheme::drawButtonHints` at `pageHeight - buttonHintsHeight` (4×106 px, `buttonPositions {38,154,268,384}`). Correction says that footer is not a product surface. Shipping M4 chrome is `M4TouchNavigation` `HeaderBack`/`BottomBackHome` + direct content taps, not bottom hint buttons. Hiding the bar on M4 restores 40-51 px content height and removes a control that promises touch zones that no longer exist. Persistence stays (no migration); only Hub row gated. |
| `frontlightBrightness` | 2 前光 (m4Only) | 前光亮度 | **keep** | `SettingsLists.h:60` → `CrossPointSettings.h:330` ; `BoardConfig.h:850` `FrontlightConfig {48 cool, 47 warm}` | Real dual PWM frontlight, M4-only flag correct. |
| `frontlightWarmth` | 2 前光 (m4Only) | 前光色温 | **keep** | `SettingsLists.h:62` → `CrossPointSettings.h:331` | Same. |
| `sleepBeforeFullRefresh` | 1 墨水屏刷新 | 关机前全刷 | **keep** | `SettingsLists.h:65` → `CrossPointSettings.h:340` | Visual. |
| `imageQuality` | 0 界面 | 图片质量 | **keep** | `SettingsLists.h:66` → `CrossPointSettings.h:361` | EP rendering. |
| `iconStyle` | 0 界面 | 图标风格 `风格一/二/三` | **decide** | `SettingsLists.h:69` → `CrossPointSettings.h:374` consumer `FengyanTheme.cpp:153` for `*32` file icons only; **zero** consumer in `HomeSceneModel`/`GfxSceneRenderer` | Only file-manager 32-px icons on M4; Home is Scene + plugin BMPs. If that chrome is not shipping, hide on M4 Hub; else keep as `文件列表图标风格`. Marked `decide`. |
| `homeIconStyle` | 0 界面 | 图标选中风格 | **hide (on M4)** | `SettingsLists.h:72` → `CrossPointSettings.h:371` — zero consumer outside persistence `CrossPointSettings.cpp:140,342,633,812` | Dead on Scene Home. No renderer branches. Persist only, gate Hub. |
| `uiFontSize` | 0 界面 (m4Only) | 系统字号 | **keep** | `SettingsLists.h:77` → `CrossPointSettings.h:225` `EpdFontLoader::applySystemChrome` | Real chrome size, `m4Only` correct. |
| `readerLayout` | 3 阅读排版 | 阅读排版 ACTION | **keep** | `SettingsActivity.cpp:96` ACTION → `EpubReaderSettingsActivity`; `SettingsHubPolicy.h:85` | Spec door to reader typography; not input-dependent. |
| `systemAnimationEnabled` | 4 翻页动画 (m4Only) | 系统动画 | **keep** | `SettingsLists.h:242` → `CrossPointSettings.h:344` | M4-only strip, keep. |
| `pageTurnAnimationSteps` | 4 翻页动画 (m4Only) | 动画步数 | **keep** | `SettingsLists.h:244` → `CrossPointSettings.h:350` | Same. |
| `pageTurnAnimationMult` | 4 翻页动画 (m4Only) | 窗口倍数 | **keep** | `SettingsLists.h:246` → `CrossPointSettings.h:352` | Same. |
| `pageTurnAnimationTp` | 4 翻页动画 (m4Only) | 动画波形TP | **keep** | `SettingsLists.h:249` → `CrossPointSettings.h:354` | Same. |
| `pageTurnAnimationFrameRate` | 4 翻页动画 (m4Only) | 动画帧率 | **keep** | `SettingsLists.h:252` → `CrossPointSettings.h:356` | LUT byte, keep. |

### 3.2 KeysOperations (Hub 1) — 6 rows, single group, no section header

| key | UI label (current) | corrected verdict | evidence | rationale (LEFT 3 + full-screen touch only) |
|-----|--------------------|-------------------|----------|---------------------------------------------|
| `remapButtons` | 重新映射前置按键 ACTION | **hide (on M4)** | `SettingsHubPolicy.h:94` `kKeysRows[0]`; `SettingsActivity.cpp:101` ACTION → `ButtonRemapActivity`; `ButtonRemapActivity.cpp:26-100` reassigns 4 `Back/Confirm/Left/Right` roles + previews via `GUI.drawButtonHints(labelForHardware(BACK)…)`; `MappedInputManager.cpp:149-171` `mapButton` via `frontButton*`, `228-229` `physical[4]` footer translation; `CrossPointSettings.h:63-77` `FRONT_BUTTON_*`; `BoardConfig.h:845` proves front pins UNASSIGNED so the four logical roles have **no physical wiring** | **Corrected**: on M4 this reorders the bottom-bar touch slots, not hardware keys. With no footer, the entire 4-role remap is a surface that does not ship. The inherited `FRONT_HW_*` indices, `frontButtonLayout` enum, and `ButtonRemapActivity` flow are legacy. Direct content taps/swipes already navigate; the remap has nothing to remap. Hide on M4; do not rewrite to `底部触摸按键映射` — that preserves the invalid surface. `shortPwrBtn` and `sideButtonLayout` already cover the only real keys. |
| `sideButtonLayout` | 侧边按钮设置（仅阅读） options `上, 下 / 下, 上` | **keep** (wording polish optional, LEFT-PHYSICAL only) | `SettingsLists.h:167` → `CrossPointSettings.h:83,211` `SIDE_BUTTON_LAYOUT` ; `MappedInputManager.cpp:24-28` `kSideLayouts[] { {UP,DOWN},{DOWN,UP} }`, `168-171` `PageBack/PageForward` routing ; `SettingsHubPolicy.h:95` | **Live** — swaps which of the LEFT `Up/Down` (GPIO1/2) means Prev/Next in readers via `PageBack/PageForward`. No footer dependency. If rewritten, wording must be `左侧翻页键布局 (仅阅读)` with options `上=上一页 / 下=下一页` — never `底部触摸按键`. Scope note "仅阅读" is accurate (`EpubReaderActivity`/`TxtReaderActivity` only). Verdict **keep**; wording fix is optional polish. |
| `shortPwrBtn` | 短按电源键 values `忽略/休眠/翻页/全刷/确认` | **keep** (wording polish optional, LEFT-PHYSICAL only) | `SettingsLists.h:171` → `CrossPointSettings.h:182,205,427` ; `main.cpp:1490-1506` maps `FULL_REFRESH→FAST_REFRESH`, `CONFIRM→injectButtonPress(BTN_CONFIRM)`; `EpubReaderActivity.cpp:854-870` `powerPageTurn` ; `HalPowerManager.cpp:97` `esp_deep_sleep_enable_gpio_wakeup(POWER_BUTTON_PIN)` | **Live** — Power is bottom of the 3 left keys and only wake source. If wording touched, say `短按电源键 (左侧底部键)` and rename options with left-key semantics `忽略 / 休眠(含10ms快醒) / 作为翻页键 / 刷新屏幕 / 作为确认键`. Product note: `休眠` shortens `getPowerButtonDuration()` 400→10 ms; `全刷` is already downgraded to `FAST_REFRESH`. None of this involves a footer. |
| `longPressChapterSkip` | 长按左右键跳章节 | **keep** (wording polish optional) | `SettingsLists.h:169` → `CrossPointSettings.h:284` ; `EpubReaderActivity.cpp:797` `usePressForPageTurn = !longPressChapterSkip`, `829-845` `isPhysicalButton && held>1000` → chapter `±1`, `894-932` BT hold `BT_HOLD_THRESHOLD=3, BT_IDLE_RESET_MS=950, BT_MIN_HOLD_MS=2000` | **Live** on LEFT page keys (>1 s hold of `PageBack/PageForward` i.e. swapped `Up/Down`) and BT repeats. Optional wording `长按翻页键跳章节` to avoid implying front Left/Right. No footer. |
| `longPressBoot` | 长按开机 | **keep** (wording polish optional) | `SettingsLists.h:174` → `CrossPointSettings.h:279` ; `main.cpp:439-476` `verifyPowerButtonDuration()` gates `longPressBoot` → requires 2 s `BTN_POWER` hold to wake from deep sleep | **Live and important** — M4 factory sleep is deep sleep with rail hold (`HalPowerManager.cpp:86` `GPIO13` latch). Without gate, pocket brush of Power wakes. Optional rewrite `唤醒需长按电源键2秒`. Left physical only. |
| `libraryLongPressMenu` | 长按确认键打开菜单 | **hide (on M4)** | `SettingsLists.h:175` → `CrossPointSettings.h:387` ; `MyLibraryActivity.cpp:768-894` : `if (SETTINGS.libraryLongPressMenu && isPressed(Confirm) && held>=700)` → `showingActionMenu=true` else `wasReleased(Confirm)` toggles menu vs open; `SettingsHubPolicy.h:99` ; `MyLibraryActivity.cpp:660` direct row tap `wasScreenTapped(tx,ty) → listIndexFromPoint → openSelected()` already opens files on content touch; `BoardConfig.h:845` proves `BTN_CONFIRM` physical never fires | **Corrected to HIDE**. Round-8 scoped it to "`Confirm` bottom slot" long-press. That slot is the-footer-that-does-not-exist. Without it, the `Confirm` paths `isPressed(Confirm)` / `wasReleased(Confirm)` have **no default reliable M4 trigger**: physical `confirm` pin is UNASSIGNED (`InputManager::isDigitalPressed(-1)==false`), footer `M4FooterTouchPolicy` hit-test is the legacy strip, and the only other `Confirm` injections are `shortPwrBtn==CONFIRM` `gpio.injectButtonPress(BTN_CONFIRM)` (opt-in) and BT HID virtual buttons — neither a shipping default. Meanwhile the file-manager already handles full-screen row tap → open via `wasScreenTapped` and row-drag `wasScreenTouchDown`, so short-tap → open does not need Confirm at all. The toggle therefore toggles a button that effectively does not exist in the shipping touch path, and its footer Hint label `confirmHint = libraryLongPressMenu ? "打开" : "菜单"` (`MyLibraryActivity.cpp:1040`) draws on the non-shipping footer (`GUI.drawButtonHints`). `SETTINGS.libraryLongPressMenu` remains provably **read** only in `MyLibraryActivity` (confirming narrow scope), but on M4 the conflicted opening path is superseded by content taps. Verdict: **hide on M4**. Re-expose only if product defines a real non-footer Confirm gesture (e.g. long-press on the tapped row) and wires it to `isPressed(Confirm)` without `M4FooterTouchPolicy`. Prove with source diff before exposing. |

### 3.3 NetworkSync (Hub 2) — unchanged, all keep

`wifiAlwaysReselect`, `autoSyncTimeOnBoot`, `bluetooth` (real NimBLE HID host `BluetoothHIDManager.h:41`, not stub), `koreader`, `jianguo`, `dataCapsule` — six keeps, no footer dependency.

### 3.4 SystemMaintenance (Hub 3) — unchanged, all keep

`systemLanguage`, `sleepTimeout`, `directTxtRead`, `clearCache`, `resetSettings`, `developerOptions` (m4Only, `HalGPIO` debug bridge), `switchBootSlot` (M4 OTA slot). No footer dependency.

### 3.5 Non-Hub (web/API-only) — correctly hidden

`Reader` category (`SettingsLists.h:87-164` `firstlineintented…pageTurnAnimationDir`) and X3-gated `tilt/tap/autoRotate` (`#ifdef CROSSPOINT_X3`) are absent from `SettingsHubPolicy.h:71-119` — intentional per spec §4.1 `不要把 Reader 目录键铺到这一页`. `landscapeDualPageEnabled` stays web-only (entered via hold of `PageBack(Up)` `EpubReaderActivity.cpp:542-572`). No wrong-`#ifdef` polarity found.

---

## 4. Proposed `hide_on_m4` list (corrected model)

Host-enforced Hub filter only — **do not delete `CrossPointSettings` fields**; rows stay readable via JSON/API, just not drawn as Hub entries on M4.

```
hide_on_m4 = [
  "buttonHintsEnabled",   // CRITICAL (corrected) — footer-painted 4-slot bar that product says does not ship; was Round-8 "rewrite"
  "remapButtons",         // CRITICAL (corrected) — remaps 4 footer logical roles with no pins on M4; was Round-8 "rewrite"
  "libraryLongPressMenu", // CRITICAL (corrected) — Confirm-slot toggle with no provable non-footer Confirm gesture; was Round-8 "rewrite scoped to Confirm slot"
  "homeIconStyle",        // CRITICAL — proven dead, zero consumer outside persistence (unchanged from Round 8)
]

decide_on_m4 = [
  "iconStyle",            // only file-manager 32-px icons via FengyanTheme.cpp:153; decide per product: keep as "文件列表图标风格" or hide if that chrome is not shipping
]
```

If product also accepts the `iconStyle` hide, DisplayReading loses two rows on M4 (`homeIconStyle` + `iconStyle`) plus `buttonHintsEnabled`, and Keys loses two (`remapButtons`, `libraryLongPressMenu`). Totals after full hide list (`homeIconStyle` + `buttonHintsEnabled` + `iconStyle`):

- DisplayReading Hub: 19 → 16; flattened 5 titles + surviving settings = 5 + (8+3+2+1+5 minus 3 hidden) ≈ 21 (was 24), window 8 still slides.
- KeysOperations Hub: 6 → 4 (only `sideButtonLayout, shortPwrBtn, longPressChapterSkip, longPressBoot`). Single-group, still fits in one screen.

Gating sketch (illustrative, host lock test covers it):

```cpp
// SettingsHubPolicy.h — the minimal Hub gate.
inline constexpr CatalogRow kDisplayRows[] = {
    {{"sleepScreen", 0}, false},
    // ...
    {{"buttonHintsEnabled", 0}, false}, // M4: filter out in settingsHubRowAt (or add m4Only=hide flag)
    // ...
    {{"iconStyle", 0}, false},          // decide: hide on M4 = filter
    {{"homeIconStyle", 0}, false},      // M4: filter
};

// Option A: add a bool hideOnM4 to CatalogRow and filter in availableCount/At.
// Option B (cheapest, no struct change): filter by name in availableAt() when m4Build.
```

Host lock test enforcing the corrected model (native, no device):

```cpp
#include "activities/settings/SettingsHubPolicy.h"

TEST(M4HideGate_Round8b, footer_rows_hidden_on_M4) {
  for (int i = 0; i < settingsHubRowCount(SettingsHubCard::DisplayReading, true, false); ++i)
    EXPECT_STRNE(settingsHubRowAt(SettingsHubCard::DisplayReading, i, true, false).key, "buttonHintsEnabled");
  for (int i = 0; i < settingsHubRowCount(SettingsHubCard::DisplayReading, true, false); ++i)
    EXPECT_STRNE(settingsHubRowAt(SettingsHubCard::DisplayReading, i, true, false).key, "homeIconStyle");
  for (int i = 0; i < settingsHubRowCount(SettingsHubCard::KeysOperations, true, false); ++i) {
    EXPECT_STRNE(settingsHubRowAt(SettingsHubCard::KeysOperations, i, true, false).key, "remapButtons");
    EXPECT_STRNE(settingsHubRowAt(SettingsHubCard::KeysOperations, i, true, false).key, "libraryLongPressMenu");
  }
}
// If iconStyle also hidden, add the same check for "iconStyle" on DisplayReading.
```

---

## 5. Legacy debt — code that still draws/requires the 4-slot footer

Separate from Settings Hub rows: these symbols/render paths **assume the bottom 4-slot bar exists**. Under the corrected product they are debt to track; removing the Hub row alone does not delete them. Listed as `file:symbol` (or file:behavior).

**Policy + edge synthesis (the footer mechanism itself)**

- `firmware/src/util/M4FooterTouchPolicy.h:M4FooterTouchPolicy` — entire namespace `LogicalButton {Back,Confirm,Left,Right}`, `maskStorage()/setMask()/enabled()`, `slotFromPoint(x,y,w,h,footerHeight)` with `buttonWidth 106`, `buttonPositions {38,154,268,384}`.
- `firmware/src/MappedInputManager.cpp:M4FooterTouchPolicy::slotFromPoint` / `physical[4]` — `MappedInputManager::wasReleased()` footer-hit synthesis `slotFromPoint(tx,ty,w,h,metrics.buttonHintsHeight)` → compare `physical[slot]==mappedHw (=SETTINGS.frontButton*)`, and `wasPressed` path gap (footer only synthesizes `wasReleased`, not `wasPressed`). `M4FooterTouchPolicy::enabled(logical)` gating.
- `firmware/src/MappedInputManager.cpp:mapButton` / `getPressedFrontButton` / `mapLabels` — four-logical routing through `SETTINGS.frontButtonBack/Confirm/Left/Right` and `labelForHardware(BTN_BACK/CONFIRM/LEFT/RIGHT)`; footer-label mapping via remap.
- `firmware/src/MappedInputManager.cpp:isPressed(Confirm)` — the `libraryLongPressMenu` `isPressed(Confirm) && held>=700` gate is footer-derived when Confirm comes from the bar.
- `firmware/src/activities/Activity.h:touchFooterButtonsMask` — `Activity::touchFooterButtonsMask()` default `0`, mask set in `Activity::onEnter()` `M4FooterTouchPolicy::setMask(touchFooterButtonsMask())` and cleared in `onExit()`. Plumbing that enables per-activity footer slot hit-testing.
- `firmware/src/activities/ActivityWithSubactivity.cpp:M4FooterTouchPolicy::setMask` — forwards subactivity mask.

**Persistence (four hardware-assigned slot indices + legacy layout enum)**

- `firmware/src/CrossPointSettings.h:FRONT_BUTTON_LAYOUT` — enum `BACK_CONFIRM_LEFT_RIGHT=0, LEFT_RIGHT_BACK_CONFIRM, LEFT_BACK_CONFIRM_RIGHT, BACK_CONFIRM_RIGHT_LEFT`.
- `firmware/src/CrossPointSettings.h:FRONT_BUTTON_HARDWARE` — `FRONT_HW_BACK=0, CONFIRM=1, LEFT=2, RIGHT=3`.
- `firmware/src/CrossPointSettings.h:frontButtonBack / frontButtonConfirm / frontButtonLeft / frontButtonRight` — four `uint8_t` remap fields (`214-217`).
- `firmware/src/CrossPointSettings.h:buttonHintsEnabled` — `uint8_t buttonHintsEnabled = 0` (326).
- `firmware/src/CrossPointSettings.cpp:frontButton*` persistence (doc/bin/pod `38-80, 143, 183-188, 269-274, 383-514, 735, 815-871` incl. `FRONT_BUTTON_LAYOUT_COUNT` legacy migration).
- `firmware/src/CrossPointSettings.cpp:buttonHintsEnabled` persistence (`143,328,596,815`).
- `firmware/src/HalGPIO.h:BTN_BACK/CONFIRM/LEFT/RIGHT` indices `0..3` — logical indices reused as footer slot indices even though they have no M4 pins (see `BoardConfig` proof above). Not dead globally (other SKUs use ADC ladder `InputManager::getDigitalState` for them), but M4-paired with footer on M4.
- `firmware/src/main.cpp:FRONT_HW injects` — `gpio.injectButtonPress(SETTINGS.frontButtonLeft/Right/Back)` via X3 `TiltDetector` action callback (`main.cpp:919-933`), gated `CROSSPOINT_X3` (correctly hidden on M4 but still references the front mapping symbols).

**Theme chrome (paints the bar)**

- `firmware/src/components/UITheme.h:drawButtonHints` — facade `GUI.drawButtonHints(renderer, btn1,btn2,btn3,btn4, force)` delegating to theme.
- `firmware/src/components/themes/BaseTheme.h:drawButtonHints` — virtual declaration.
- `firmware/src/components/themes/fengyan/FengyanTheme.h:drawButtonHints` / `firmware/src/components/themes/fengyan/FengyanTheme.cpp:454:FengyanTheme::drawButtonHints` — draws four `fillRect+drawRect` footers at `pageHeight - buttonHeight`, `buttonWidth 106`, `buttonPositions {38,154,268,384}`, gated by `buttonHintsEnabled`.
- `firmware/src/components/themes/lyra/LyraTheme.h:drawButtonHints` / `firmware/src/components/themes/lyra/LyraTheme.cpp:288:LyraTheme::drawButtonHints` — portrait-locked 4-slot variant, same gating.
- `firmware/src/util/M4ErrorScreen.h:GUI.drawButtonHints` — error-screen footer strip (uses `force=true`, so ignores the hide toggle).
- `firmware/src/util/M4TouchNavigation.h:drawButtonHints→BottomBackHome` — comment `GUI.drawButtonHints are automatically promoted to BottomBackHome`, tying footer drawing to navigation mode.

**Activity that owns/assumes the 4-slot UI**

- `firmware/src/activities/settings/ButtonRemapActivity.h:ButtonRemapActivity` / `firmware/src/activities/settings/ButtonRemapActivity.cpp:ButtonRemapActivity` — entire flow reassigns `tempMapping[4]` of `FRONT_HW_*`, waits on `getPressedFrontButton()` (`wasPressed(BTN_*)`), previews via `GUI.drawButtonHints(renderer, labelForHardware(FRONT_HW_BACK)…)`. Includes side `Up=reset / Down=cancel` handling (which are the real left keys used to drive the footer remap — inverted layering).
- `firmware/src/activities/home/HomeActivity.h:touchFooterButtonsMask` — `return Confirm|Left|Right` (footer optimism on Home chrome).
- `firmware/src/activities/apps/AppListActivity.h:touchFooterButtonsMask` / `firmware/src/activities/apps/AppListActivity.cpp:M4FooterTouchPolicy::setMask` — activity-owned `Back|Confirm|Right|(Left if plugin)` mask.
- `firmware/src/activities/apps/NativeProviderLoginActivity.h:touchFooterButtonsMask` — `Back|Confirm` + `isFullscreenActivity=true`.
- `firmware/src/activities/apps/NativeProviderBookActivity.h:touchFooterButtonsMask` / `firmware/src/activities/apps/NativeProviderBookActivity.cpp:M4FooterTouchPolicy::setMask` — `Back|Confirm|Left` (fullscreen) + `NativeProviderBookActivity.cpp:228` direct `slotFromPoint` call.
- `firmware/src/activities/apps/NativeProviderEndpointActivity.h:touchFooterButtonsMask` / `firmware/src/activities/apps/NativeProviderEndpointActivity.cpp:touchFooterButtonsMask` + `slotFromPoint` — fullscreen `Back|Confirm|Left|Right`.
- `firmware/src/activities/settings/ResetSettingsActivity.h:touchFooterButtonsMask` + `.cpp:GUI.drawButtonHints` — `Back|Confirm`.
- `firmware/src/activities/settings/ClearCacheActivity.h:touchFooterButtonsMask` + `.cpp:GUI.drawButtonHints` — `Back|Confirm`.
- `firmware/src/activities/settings/OnlineOtaActivity.h:touchFooterButtonsMask` + `.cpp:GUI.drawButtonHints` — `Back|Confirm` with forced hints.

**Pervasive draw callers (content-size + chrome overlap — all `GUI.drawButtonHints` sites)** — under no-footer model these are the screens whose `availableHeight` and footer overlay assume a bottom strip. Representative set (193 grep hits total; full list is `grep -rn drawButtonHints firmware/src --include="*.cpp"`):

- `firmware/src/activities/home/MyLibraryActivity.cpp:GUI.drawButtonHints` (library list footer `Back|confirmHint|上移|下移`) + `MyLibraryActivity.h:touchFooterButtonsMask` implicit.
- `firmware/src/activities/home/HomeActivity.cpp:GUI.drawButtonHints` (Home chrome).
- `firmware/src/activities/browser/OpdsBookBrowserActivity.cpp:GUI.drawButtonHints`, `JianGuoBrowserActivity.cpp`, `DataCapsuleBrowserActivity.cpp`, `CalibreConnectActivity.cpp`, `NetworkModeSelectionActivity.cpp`, `WifiSelectionActivity.cpp`, `CrossPointWebServerActivity.cpp`, `ScreenBridgeActivity.cpp`, `AppInstallActivity.cpp`, `AppListActivity.cpp`.
- `firmware/src/activities/reader/EpubReaderMenuActivity.cpp:GUI.drawButtonHints` (+ `listHeight` reserve via `buttonHintsHeight`), `EpubReaderSettingsActivity.cpp`, `EpubReaderChapterSelectionActivity.cpp`, `TxtReaderChapterSelectionActivity.cpp`, `BookmarkManagerActivity.cpp`, `BookmarkNotesActivity.cpp`, `XtcReaderMenuActivity.cpp`, `KOReaderSyncActivity.cpp`, `JianGuoSyncActivity.cpp`, `TiltPageTurnSettingsActivity.cpp`, `AutoPageTurnIntervalActivity.cpp`.
- `firmware/src/activities/settings/CalibreSettingsActivity.cpp:GUI.drawButtonHints`, `KOReaderAuthActivity.cpp`, `NumberSelectionActivity.cpp`, `ResetSettingsActivity.cpp`, `ClearCacheActivity.cpp`, `DataCapsuleSettingsActivity.cpp`, `FontSelectionActivity.cpp`, `OtaUpdateActivity.cpp`, `SimpleBluetoothActivity.cpp`, `JianGuoYunSettingsActivity.cpp`, `KOReaderSettingsActivity.cpp`, `DeveloperOptionsActivity.cpp`, `OnlineOtaActivity.cpp`.
- Layout-coupled consumers of `UITheme::getMetrics().buttonHintsHeight` that reserve footer space even when no button there: `BookmarkManagerActivity.cpp:55`, `EpubReaderMenuActivity.cpp:474,890`, `TiltPageTurnSettingsActivity.cpp:214,277`, `EpubReaderSettingsActivity.cpp:97,241`, `MyLibraryActivity.cpp` `contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - …`, `M4ErrorScreen.h:119` `footerReserve`, `UITheme.cpp:41`, `M4UiStyle.h:61,132`.

Note: not all of these are "bugs" today — many screens run on legacy `GUI.drawList/drawHeader` paths where footer chrome is the only Back/Confirm affordance. The new M4 shipping path is `M4TouchNavigation` (`HeaderBack` for normal activities, `BottomBackHome` BottomBackHome, `ChapterHeaderBack`) + content `wasScreenTapped/listIndexFromPoint/readerZoneFromPoint`. The migration plan for each caller is: replace its `GUI.drawButtonHints` + `buttonHintsHeight` reserve with the activity's `showTouchNavigation()` / navigation-bar-provided Back/Home, and keep `wasScreenTapped` row/zone taps. The debt list above gives the grep anchors (`M4FooterTouchPolicy` / `drawButtonHints` / `buttonHintsHeight` / `frontButton*`) to audit per-screen without reading the whole tree.

---

## 6. Invalidated Round-8 rewrite suggestions

These Round-8 proposals assumed the four bottom touch zones ship on M4. Under the correction they **must not** be applied; the right action is **hide**, not rewrite-to-footer. Keeping them would double down on the invalid surface the human correction forbids (`"DO NOT rewrite labels to say 底部触摸按键"`).

| Round-8 proposal | Why it is invalid |
|------------------|-------------------|
| `buttonHintsEnabled` rewrite → `底部触摸按键提示 (Bottom touch bar hints)`, default `On` | Ships a toggle for a bar the product says does not exist. On M4 the 4-slot hints are the footer itself; flipping the default to On just hides less of the non-shipping chrome. **Invalidated**. Correct: hide row on M4; delete or gate the footer paint, don't rename it. |
| `remapButtons` rewrite → `底部触摸按键映射 / 触摸快捷键布局` | Renames a 4-role footer remap as if the footer were the product gesture surface. Physical M4 wiring proves it is not (front pins UNASSIGNED). **Invalidated**. Correct: hide on M4. Side-key layout (`sideButtonLayout`) and Power (`shortPwrBtn`) already cover real keys. |
| `libraryLongPressMenu` rewrite scoped to "`Confirm` bottom slot long-press — `文件管理: 长按确认键的操作`" | Scopes to Confirm-as-footer-slot. Without footer, `Confirm` has no default M4 hit region (see §3.2). Proposing footer-scoped copy endorses the footer model. **Invalidated**. Correct: hide until product proves a real non-footer Confirm gesture (e.g. row-long-press) source-mapped without `M4FooterTouchPolicy`. |
| Ancillary Round-8 copy: `ButtonRemapActivity` footer synthesis → listen on `wasReleased` / "触摸快捷键" | Treats `wasReleased` footer synthesis as shipping input to be polished. Under correction the synthesis itself is legacy debt (`MappedInputManager.cpp:219-229` footer-as-synthetic-`wasReleased`). Product asks to hide the remap, not fix its tap plumbing. **Invalidated** as a required fix; kept only as debt note if someone temporarily unhides the mask. |
| Any Hub label/option change that inserts `底部触摸按键` / `前置按键→触摸按键` for M4 | All footer-phrased labels are invalidated. The surviving rewrites are **left-edge only** (`左侧翻页键布局`, `短按电源键 (左侧底部键)`, `唤醒需长按电源键2秒`), with no mention of footer slots. |

**What survives from Round-8 rewrites** (still valid, but must use LEFT-PHYSICAL wording only):

- `sideButtonLayout` → `左侧翻页键布局 (仅阅读)` with options `上=上一页 / 下=下一页` — **not** `底部`.
- `shortPwrBtn` → `短按电源键 (左侧底部键)` + options clarifying `休眠(10ms快醒)` / `刷新屏幕` / `作为确认键` — describes the bottom of the 3 left keys, not a footer.
- `longPressBoot` → `唤醒需长按电源键2秒` — left Power wake gate.
- `longPressChapterSkip` → `长按翻页键跳章节` — left page keys.

These four survive only as optional polish; their verdicts are **keep** (hide is not needed — the feature is live on left hardware). If product skips the wording pass entirely, shipping `keep` verbatim is also correct.

---

## 7. Open questions that remain after the correction

1. **Home vs file-manager icon styling (`iconStyle`)** — Still the same as Round 8. `FengyanTheme.cpp:153` is the only live consumer (file-manager 32-px icons). If M4 ships Scene Home (plugin BMPs) + file-manager 24-px set, should `iconStyle` stay on the Hub? Keep as `文件列表图标风格` or hide and leave the single 24 set. Needs product on whether 32-px theming is a feature.

2. **Power→Confirm injection scope (`shortPwrBtn==CONFIRM` `main.cpp:1505`)** — With footer gone, `shortPwrBtn==CONFIRM` becomes the *only* built-in non-BT way to fire `Confirm` outside a content tap. Does product intend Power-as-Confirm to work inside the file manager where `libraryLongPressMenu` used to key on Confirm? If `libraryLongPressMenu` is hidden and row-tap opens directly, Power→Confirm is harmless but mostly redundant with content taps. Confirm intent on file-manager rows (via row-tap long-press gesture vs button) should be clarified.

3. **File-manager open/menu with no Confirm button** — Row `wasScreenTapped` → `openSelected()` already opens on direct row tap, so short-tap→open is preserved without Confirm. What is the intended **menu** affordance now? Prior `libraryLongPressMenu` used `isPressed(Confirm) held>=700` to open the action menu; without footer Confirm, menu must be row-long-press or a content chevron, not a footer button-hint toggle. If product wants a menu-vs-open toggle to remain, it needs a new non-footer gesture wired to `showingActionMenu` — do not reuse `isPressed(Confirm)`.

4. **Reader swipe/page keys vs button remap** — Reader already navigates via swipe, `Up/Down` (`PageBack/PageForward`), tap zones (`TouchHitGeometry::readerZoneFromPoint`), and BT virtual keys. With `remapButtons` hidden, there is no remap for reader navigation — but none is needed; reader `EpubReaderActivity.cpp:797,829,894` hard-codes `PageBack/PageForward` routing via `sideButtonLayout`. Confirm: does any reader screen still depend on `LEFT/RIGHT` logical roles except via swipe?

5. **Footer-space reclamation per activity** — Many screens reserve `buttonHintsHeight` (40–51 px) from `contentHeight`/`listHeight` (e.g. `MyLibraryActivity`, `EpubReaderMenuActivity`, `BookmarkManagerActivity`). With footer hidden, that space should return to the list/paged content. The Settings Hub/L2 Scene spec already pins `contentBottom = screenHeight - footerHeight` with `footerHeight` for Settings, but the legacy list renderers (`GUI.drawList`) still subtract `buttonHintsHeight` unconditionally. Product should confirm whether legacy list screens keep the footer blank or grow the list by one row when hints are hidden on M4.

6. **Deprecation plan for persistence** — The hide list gates only the Hub rows. `CrossPointSettings.frontButtonBack/Confirm/Left/Right`, `FRONT_BUTTON_LAYOUT`, and `buttonHintsEnabled` remain in doc/bin JSON for migration, and `HalGPIO::BTN_BACK..RIGHT` indices stay for non-M4 SKUs. When does the M4 build drop the `FRONT_HW` enum and per-activity `touchFooterButtonsMask` entirely, versus keeping them for API-compat? No action this round — but the debt in §5 should have a dated removal intent so web/API no longer writes footer indices for M4.

---

## 8. Evidence appendix — file:symbol index for every hide/rewrite traced

| key | primary field | Hub gating | proof it matters / does not matter vs LEFT 3 + full-screen touch |
|-----|---------------|------------|------------------------------------------------------------------|
| `buttonHintsEnabled` | `CrossPointSettings.h:326` `buttonHintsEnabled` | `SettingsLists.h:57` `Toggle` ; `SettingsHubPolicy.h:77` | **Gate** `LyraTheme.cpp:290` / `FengyanTheme.cpp:456` draws 4×106 footer; product says that footer does not ship. |
| `remapButtons` (ACTION) | `CrossPointSettings.h:214-217` `frontButton*` + `63-77` `FRONT_BUTTON_*` enums | `SettingsHubPolicy.h:94` `kKeysRows[0]` ; `SettingsActivity.cpp:101` → `ButtonRemapActivity` | **Footer-dependent** `ButtonRemapActivity.cpp:175` draws `labelForHardware(FRONT_HW_*)` via `GUI.drawButtonHints`; `MappedInputManager.cpp:149-171,228-229` routes through `M4FooterTouchPolicy`. |
| `libraryLongPressMenu` | `CrossPointSettings.h:387` `libraryLongPressMenu` | `SettingsLists.h:175` ; `SettingsHubPolicy.h:99` | **Footer-derived** `MyLibraryActivity.cpp:768` `isPressed(Confirm) held>=700`; `BoardConfig.h:845` proves Confirm never fires physically; row tap `wasScreenTapped→openSelected()` already opens without Confirm. |
| `sideButtonLayout` | `CrossPointSettings.h:83,211` `SIDE_BUTTON_LAYOUT` | `SettingsLists.h:167` ; `SettingsHubPolicy.h:95` | **Left-hardware** `MappedInputManager.cpp:24-28` `kSideLayouts { {UP,DOWN},{DOWN,UP} }` ; no footer. |
| `shortPwrBtn` | `CrossPointSettings.h:182,205,427` `SHORT_PWRBTN` / `getPowerButtonDuration()` | `SettingsLists.h:171` ; `SettingsHubPolicy.h:96` | **Left Power** `main.cpp:1490-1506` + `HalPowerManager.cpp:97` wake; not a footer slot. |
| `longPressBoot` | `CrossPointSettings.h:279` | `SettingsLists.h:174` ; `SettingsHubPolicy.h:98` | `main.cpp:439-476` `verifyPowerButtonDuration()` wake gate on `BTN_POWER`. |
| `longPressChapterSkip` | `CrossPointSettings.h:284` | `SettingsLists.h:169` ; `SettingsHubPolicy.h:97` | `EpubReaderActivity.cpp:829,894` hold >1 s on `PageBack(Up)/PageForward(Down)` plus BT. |
| `homeIconStyle` | `CrossPointSettings.h:371` | `SettingsLists.h:72` ; `SettingsHubPolicy.h:82` | **Dead** — zero consumer outside `CrossPointSettings.cpp:140,342,633,812` persistence; proving hide. |
| `iconStyle` | `CrossPointSettings.h:374` | `SettingsLists.h:69` ; `SettingsHubPolicy.h:82` | **Narrow** consumer `FengyanTheme.cpp:153` for `*32` file icons only; none in `HomeSceneModel`/`GfxSceneRenderer`. |
| M4 wiring | — | `BoardConfig.h:845-847` | `InputPins { -1,-1,-1,-1, 1,2,0, false }` + `InputManager.cpp:235-262` `isDigitalPressed(-1)==false` proves `BTN_BACK/CONFIRM/LEFT/RIGHT` never fire physically on M4 — the "front 4" are virtual/legacy. |
| Touch | — | `BoardConfig.h:848` + `HalGPIO.cpp:76` | `TouchConfig Ft6x36 {13,12,44, powerEnable 45, swapXY+flipY}` ; `hasTouch()` true. |

### 8.1 Symbol search anchors for debt audits

- Footer policy: `M4FooterTouchPolicy` → `firmware/src/util/M4FooterTouchPolicy.h`
- Footer paint: `drawButtonHints` → 193 hits; start with `UITheme.h`, `BaseTheme.h`, `FengyanTheme.*`, `LyraTheme.*`
- Mask plumbing: `touchFooterButtonsMask` → `Activity.h`, `ActivityWithSubactivity.cpp`, per-activity overrides in `HomeActivity.h`, `AppListActivity.h`, `NativeProvider*.h`, `Reset/ClearCache/OnlineOtaActivity.h`
- Persistence: `frontButton` / `buttonHints` → `CrossPointSettings.h/.cpp`
- BT virtual confirm (surviving non-footer Confirm source): `gpio.injectButtonPress(BTN_CONFIRM)` → `main.cpp:1505` + `BluetoothHIDManager.cpp`

---

## 9. What the commit does and does not do

- **Does**: writes this REPORT at `docs/orchestration/rounds/round-8b-muse-settings-no-footer-slots-REPORT.md` and reclassifies `buttonHintsEnabled → hide`, `remapButtons → hide`, `libraryLongPressMenu → hide` under the no-footer model; lists footer debt; marks invalidated Round-8 rewrites; retains left-edge keys as **keep** with left-physical wording only where touched.
- **Does not**: change Hub filtering (`SettingsHubPolicy.h`), `CrossPointSettings` fields, theme footer painting, `Activity.h` mask plumbing, or any `GUI.drawButtonHints` caller. Those are debt tracked in §5 for a later migration that replaces footer hints with `M4TouchNavigation` (`HeaderBack`/`BottomBackHome`) + content-row taps.
- **No flash, no push, no reset/clean** — per task contract.

---

*Forbidden actions not taken: `git push origin`, hardware flash, `git reset --hard`, `git clean`, `pkill -f m4adb`, Scene/Home theme rewrite. Report is English. Commit: `round-8b(muse): correct settings audit — no M4 footer 4-slots`.*
