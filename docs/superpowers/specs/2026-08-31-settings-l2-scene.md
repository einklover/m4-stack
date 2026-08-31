# Murphy M4 Settings Hub + L2 — Scene 极简稿

日期：2026-08-31  
状态：本轮绑定设计。IA 分组继承 `39774f7` / `SettingsHubPolicy.h`；**视觉语法不再用 ChatGPT 效果图的描边分组卡片**。  
权威线框（480×800，代码绘制，不是模型图）：

- `docs/orchestration/assets/settings-hub-wireframe.png`
- `docs/orchestration/assets/settings-l2-display-wireframe.png`

实现工作区：lane 在 `m4-home-muse-impl` / `m4-home-luna-audit` / `m4-home-muse-tests`；合入 `m4-home-orch-integration`。协调者树 `m4-critical-ui-home` 不施工固件。

## 1. 目标

把设备设置从 3-tab（显示 / 按钮 / 系统）换成：

1. **Hub（一级）**：四个全宽文字行，进入二级分类。
2. **L2（二级）**：该分类下的设置行 + 分组标题。分组标题不可选。行数超过 8 时窗口滑动，不抬高 Scene `kMaxRepeatItems`。

画法复用桌面已经落地的 Scene 节点：`clear` / `text` / `line` / `battery` / `repeat`，选中条用 **填充** `round_rect`（`r=0`，`fill=true`，无 `stroke`）。页脚仍由 `GUI.drawButtonHints` 叠在 Scene 之上，theme JSON 不画 footer 节点（与 Home 一致）。

## 2. 视觉硬约束

允许：白底、黑字、1px 顶栏发丝线、4px 实心选中条、文字层次（`ui_16_bold` 标题 / `ui_20_bold` Hub 行 / `ui_16_regular` 设置名 / `ui_14_regular` 分组名与右侧值）。

禁止（JSON 与 paint 都禁）：

- `icon` / `cover` / `progress` 节点
- 任何 `stroke > 0` 的卡片、分组框、行框
- 行间 1px 分隔线
- chevron / 开关图形 / 分组容器圆角
- 反相整行填黑
- 把 `FengyanTheme::drawTabBar` / `drawList` 改成设置页长相
- 把 mock `themes/settings-scene-mock`（5 行带描边卡片 + toggle 图标）当产品

选中：**只画行左侧 4×(行高-24) 的竖条**。Hub 与 L2 同一语法。

## 3. 几何（480×800，与 Home chrome 对齐）

| 元素 | rect / 线段 | 字体 |
|---|---|---|
| 标题 | `[24,24,280,24]` | `ui_16_bold` 左对齐。Hub 文案「系统设置」；L2 文案为当前分类名 |
| 电池 | `[432,24,24,12]` | `$system.battery` |
| 发丝线 | `(23,52)–(456,52)` width 1 | 与 Home 同一条 |
| 内容顶 | `y=68` | |
| 内容底 | `y≤736` | 为 footer 留空。Scene 不画 footer |
| Hub `repeat` | `x=24 y=68 item_width=432 item_height=140 gap=12 limit=4` | 垂直 |
| Hub 选中条 | 子节点 `[0,20,4,100]` fill | `visible_if $item.selected` |
| Hub 标题 | 子节点 `[24,52,380,36]` | `ui_20_bold` `$item.title` |
| L2 `repeat` | `x=24 y=68 item_width=432 item_height=80 gap=4 limit=8` | 垂直 |
| L2 选中条 | `[0,12,4,56]` fill | `visible_if $item.selected` |
| L2 设置名 | `[20,28,260,24]` | `ui_16_regular` `$item.title` `visible_if $item.is_row` |
| L2 分组名 | `[20,32,400,20]` | `ui_14_regular` `$item.title` `visible_if $item.is_section` |
| L2 值 | `[280,30,140,20]` 右对齐 | `ui_14_regular` `$item.value` `visible_if $item.is_row` |

`kMaxRepeatItems=8` 不可改。Hub 正好 4 行。L2 窗口恒为 8 个 flattened 槽（含分组标题）。

## 4. IA（设备展示；Web/Reader 分类字符串冻结）

四个 Hub 卡，简体标题固定：

| Index | `SettingsHubCard` | 标题 | I18n |
|---|---|---|---|
| 0 | `DisplayReading` | 显示与阅读 | `kHubDisplayReading` |
| 1 | `KeysOperations` | 按键与操作 | `kHubKeysOps` |
| 2 | `NetworkSync` | 网络与同步 | `kHubNetworkSync` |
| 3 | `SystemMaintenance` | 系统与维护 | `kHubSystemMaint` |

不要复用 `kCategoryDisplay`（「1)显示」）。

### 4.1 显示与阅读

分组标题在 flattened 列表里占一行，不可选。单分组的卡可以省略标题。本卡有 5 组，必须出标题。

**界面** `kSectionUiChrome`

- `sleepScreen`, `statusBar`, `hideBatteryPercentage`, `buttonHintsEnabled`, `imageQuality`, `iconStyle`, `homeIconStyle`, `uiFontSize`

**墨水屏刷新** `kSectionEinkRefresh`

- `refreshFrequency`, `neverFullRefresh`, `sleepBeforeFullRefresh`

**前光** `kSectionFrontlight`（仅 M4；非 M4 整组省略）

- `frontlightBrightness`, `frontlightWarmth`

**阅读** `kSectionReaderDoor`

- `readerLayout` 设备专用 ACTION，**不得**写入 `getSettingsList()`。打开已有 `EpubReaderSettingsActivity`。I18n `kReaderLayout` = 「阅读排版」。不要把 Reader 目录键铺到这一页。

**翻页动画** `kSectionPageTurn`（catalog 仍是 Web 的 `System`）

- `systemAnimationEnabled`, `pageTurnAnimationSteps`, `pageTurnAnimationMult`, `pageTurnAnimationTp`, `pageTurnAnimationFrameRate`

`pageTurnAnimationEnabled` / `pageTurnAnimationDir` 留在 Reader，不上 L2。  
`uiFontSize` 循环后仍调用 `EpdFontLoader::applySystemChrome`。  
`pageTurnAnimationFrameRate` 保持 DynamicEnum / LUT `0x22/0x44/0x88`，渲染走 `valueGetter()`。

M4 flattened 计数：5 个标题 + 8+3+2+1+5 个设置 = **24**。窗口 8，必须滑动。

### 4.2 按键与操作

只有一组，**省略**分组标题。顺序：

- `remapButtons` ACTION → `ButtonRemapActivity`
- `sideButtonLayout`, `shortPwrBtn`, `longPressChapterSkip`, `longPressBoot`, `libraryLongPressMenu`

X3 倾斜/旋转键不上 M4。共 6 行，一屏放下。

### 4.3 网络与同步

**网络** `kSectionNetworkToggles`：`wifiAlwaysReselect`, `autoSyncTimeOnBoot`  
**同步入口** `kSectionSyncDoorways`：`bluetooth`, `koreader`, `jianguo`, `dataCapsule`（已有 Activity）

禁止：伪造 Wi-Fi 已连接、存储、电池、插件、日志、About、OPDS。  
2 标题 + 6 设置 = **8**，刚好一窗。

### 4.4 系统与维护

**系统** `kSectionSystem`：`systemLanguage`, `sleepTimeout`, `directTxtRead`  
**维护** `kSectionMaintenance`：`clearCache`, `resetSettings`, `developerOptions`（M4）, `switchBootSlot`（M4，值文本仍是 `runningOtaLabel()`）

**永远不要** `sdOta` / `SdOtaUpdateActivity` / `SdMan.exists("/update/firmware.bin")`。  
M4：2 标题 + 7 设置 = **9**，需要窗口。

## 5. 窗口滑动（L2 核心）

Scene 不能把 24 行一次放进 `repeat`。Adapter 把 flattened 列表切成 8 槽：

```
selectedRow     // 只在「设置行」上走，跳过 section
flatIndex(card, selectedRow) -> 该设置在 flattened 中的下标
windowStart = clamp so [windowStart, windowStart+8) 含 flatIndex
visible[i] = flattened[windowStart+i]   i=0..7；不足补空且 is_row=false, selected=false
```

上/下：在设置行集合里 wrap。经过分组标题时标题仍可能出现在窗口里，但选中条只打在设置行。  
触击：命中 section 槽 → 忽略；命中设置槽 → 选中并激活（与现网列表「点即开」一致）。  
从 NumberSelection / 子 Activity 返回：恢复 `hub` + `selectedRow`（不是回到 Hub，也不是回到 0，除非本来就是 0）。

Hub 不滑动。`selectedSettingIndex==0` 的 tab-focus 删除。长按上/下不再切分类。

## 6. Theme 包

两个 JSON，用现有 `firmware/tools/compile_home_theme.py`：

- `themes/murphy-settings/hub.json` → `firmware/src/generated/murphy_settings_hub_m4theme.h`
- `themes/murphy-settings/l2.json` → `firmware/src/generated/murphy_settings_l2_m4theme.h`

绑定 / 动作 ID（1..254，避开 1,2,10-15,20,21,30-35）：

| 符号 | ID | 用途 |
|---|---|---|
| `$system.battery` | 1 | 与 Home 相同 reserved battery |
| `$page.title` | 70 | L2 顶栏；Hub 可写死「系统设置」 |
| `$hub.cards` | 71 | Hub repeat source |
| `$page.rows` | 72 | L2 repeat source |
| `$item.title` | 73 | |
| `$item.value` | 74 | |
| `$item.selected` | 75 | 非 0 则画选中条 |
| `$item.is_section` | 76 | |
| `$item.is_row` | 77 | |
| `$item.id` | 78 | Hub 卡 index 文本或 L2 key |
| `open_hub_card` | 40 | Hub Confirm/tap |
| `activate_setting` | 41 | L2 Confirm/tap |

JSON 里 `repeat.limit` Hub=4、L2=8。不要 `icon`/`cover`/`progress`。`round_rect` 必须 `fill: true` 且无 `stroke`（或缺省 0）。

## 7. 产品 Model（不是 mock）

新建 `firmware/src/activities/settings/SettingsSceneModel.h`，模式抄 `Home` / 可参考 `SettingsSceneMockModel`，但：

- `kMaxWindowRows = 8`（不是 mock 的 5）
- 无 heap、无 Arduino、无文件系统指针跨 UI
- snapshot：`pane`、`hub`、`battery`、`title`、窗口 8 行的 title/value/flags/key

`SettingsHubPolicy.h`（从 adaptation 移植并扩展）负责：卡顺序、key 分组、flattened、windowStart、hit 几何、nav 状态。禁止 `#include SettingsActivity.h`。

建议 API（Luna 按此写 Activity，Muse 按此实现 header）：

```cpp
enum class SettingsHubCard : uint8_t {
  DisplayReading = 0,
  KeysOperations = 1,
  NetworkSync = 2,
  SystemMaintenance = 3,
};
enum class SettingsPane : uint8_t { Hub, Category };
struct SettingsNavState {
  SettingsPane pane = SettingsPane::Hub;
  SettingsHubCard hub = SettingsHubCard::DisplayReading;
  int selectedRow = 0;   // setting-row index inside current card
  int windowStart = 0;   // flattened window origin
};

constexpr int kSettingsHubCardCount = 4;
constexpr int kSettingsL2Window = 8;
constexpr int kSettingsContentTop = 68;
constexpr int kSettingsHubItemH = 140;
constexpr int kSettingsHubGap = 12;
constexpr int kSettingsL2ItemH = 80;
constexpr int kSettingsL2Gap = 4;

enum class SettingsFlatKind : uint8_t { Section, Setting };
struct SettingsFlatRow {
  SettingsFlatKind kind = SettingsFlatKind::Setting;
  uint8_t section = 0;
  const char* key = "";
  const char* titleZh = "";
};

int settingsHubRowCount(SettingsHubCard, bool m4Build, bool x3Build);
SettingsHubRow settingsHubRowAt(SettingsHubCard, int index, bool m4Build, bool x3Build);
int settingsFlatCount(SettingsHubCard, bool m4Build);
SettingsFlatRow settingsFlatAt(SettingsHubCard, int flatIndex, bool m4Build);
int settingsFlatIndexOfSetting(SettingsHubCard, int settingIndex, bool m4Build);
int settingsWindowStart(int flatIndex, int flatCount, int window = kSettingsL2Window);

SettingsNavState settingsNavOpenCard(SettingsNavState, SettingsHubCard);
SettingsNavState settingsNavBack(SettingsNavState);
SettingsNavState settingsNavMoveHub(SettingsNavState, int delta);
SettingsNavState settingsNavMoveRow(SettingsNavState, int delta, int settingCount);
SettingsNavState settingsNavSyncWindow(SettingsNavState, SettingsHubCard, bool m4Build);
```

`settingsNavMoveRow` 必须跳过 section。`settingsNavSyncWindow` 在每次选中变化后更新 `windowStart`。

## 8. SettingsActivity 职责（Luna）

- 去掉 `categoryCount=3`、`categoryNames`、`GUI.drawTabBar`、`selectedSettingIndex==0` tab-focus。
- `pane==Hub` 渲染 hub 包；`pane==Category` 渲染 l2 包。都走 `GfxSceneRenderer`，不要 `GUI.drawList`。
- overlay `GUI.drawHeader` **不要**再画第二套顶栏——标题已在 Scene 里。只 overlay `GUI.drawButtonHints`。
- Confirm / tap：Hub → open card；L2 → 现有 toggle/enum/value/ACTION。ACTION 目标保持：`ButtonRemapActivity`、`SimpleBluetoothActivity`、`KOReaderSettingsActivity`、`JianGuoYunSettingsActivity`、`DataCapsuleSettingsActivity`、`ClearCacheActivity`、`ResetSettingsActivity`、`DeveloperOptionsActivity`、`NumberSelectionActivity`、新 `readerLayout` → `EpubReaderSettingsActivity`。
- Back：L2→Hub（恢复卡选中），Hub→Home（现有 `SETTINGS.saveToFile()` + 字体 reload）。
- 删除 `sdOta` 分支。保留 `switchBootSlot`。
- 不要改 `SettingsLists.h` 分类字符串、不要改 `CrossPointWebServer.cpp`、不要改 `FengyanTheme::drawTabBar/drawList`。

## 9. 明确非目标

- 不把 ChatGPT Reading/Device/About 信息架构搬过来。
- 不在 L2 倾倒 Reader 目录。
- 不接 OPDS。
- 不改 `kMaxRepeatItems` / `kMaxSceneNodes`。
- 不把 mock 5 行设置 Scene 接进生产 Activity。
- 不刷机、不 `git push origin`、不碰协调者脏树。

## 10. 验收

Host：policy 分组与 key 成员、flattened 计数（Display M4=24、Network=8、System M4=9、Keys=6）、窗口始终 ≤8 且含当前选中、theme JSON 无 icon/cover/progress/stroke、几何常量与 JSON 一致、源码扫描 SettingsActivity 无 `sdOta` / `drawTabBar`。  
QEMU 由协调者在合入后跑，子代理不要抢 PTY。
