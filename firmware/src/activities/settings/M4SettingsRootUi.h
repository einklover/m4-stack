#pragma once

#include <cstdio>
#include <cstring>

#include "activities/settings/M4SettingsAction.h"
#include "activities/settings/M4SettingsCatalog.h"
#include "activities/settings/M4SettingsConfirm.h"
#include "activities/settings/M4SettingsFocus.h"
#include "activities/settings/M4SettingsNav.h"

// Host-testable Settings root session. Consumes Foundation APIs; does not
// change Foundation signatures. Root is the §4.1 8-row catalog (no Hub).

enum class M4SettingsPageKind : uint8_t { Root, ChildList, Choice, Number, Confirm };

struct M4SettingsUiState {
  M4SettingsPageKind page = M4SettingsPageKind::Root;
  M4SettingsPageKind returnPage = M4SettingsPageKind::Root;
  char selectedKey[32]{};
  char parentKey[32]{};
  char choiceKey[32]{};
  char listStack[32]{};
  int selectedSlot = 0;
  int windowStart = 0;
  int choiceCount = 0;
  int choiceChecked = 0;
  int burstCount = 0;
  bool editorCommitted = false;
  bool confirmAccepted = false;
  bool confirmCancelled = false;
  bool bootSlotSwitchRequested = false;
};

namespace m4_settings_root_ui_detail {

constexpr M4SettingsRow kFrontlight[] = {
    {"frontlightBrightness", "亮度", M4SettingsControl::Number, true},
    {"frontlightWarmth", "色温", M4SettingsControl::Number, true},
};

constexpr M4SettingsRow kKeys[] = {
    {"remapButtons", "按键映射", M4SettingsControl::Navigate, true},
    {"sideButtonLayout", "侧键布局", M4SettingsControl::Choice, true},
    {"shortPwrBtn", "短按电源", M4SettingsControl::Choice, true},
    {"longPressChapterSkip", "长按跳章", M4SettingsControl::Toggle, true},
    {"longPressBoot", "长按开机", M4SettingsControl::Toggle, true},
    {"libraryLongPressMenu", "书库长按菜单", M4SettingsControl::Toggle, true},
};

constexpr M4SettingsRow kMaintenance[] = {
    {"clearCache", "清理缓存", M4SettingsControl::Confirm, true},
    {"resetSettings", "重置设置", M4SettingsControl::Confirm, true},
    {"developerOptions", "开发者选项", M4SettingsControl::Navigate, true},
    {"switchBootSlot", "切换启动槽", M4SettingsControl::Confirm, true},
};

constexpr M4SettingsRow kAdvanced[] = {
    {"advancedDisplay", "显示与刷新", M4SettingsControl::Navigate, true},
    {"advancedChrome", "界面与字体", M4SettingsControl::Navigate, true},
    {"advancedAnim", "动画", M4SettingsControl::Navigate, true},
    {"advancedConnect", "连接与同步", M4SettingsControl::Navigate, true},
    {"advancedSystem", "系统", M4SettingsControl::Navigate, true},
};

constexpr M4SettingsRow kAdvancedDisplay[] = {
    {"statusBar", "阅读进度", M4SettingsControl::Choice, true},
    {"hideBatteryPercentage", "隐藏电池百分比", M4SettingsControl::Choice, true},
    {"refreshFrequency", "刷新频率", M4SettingsControl::Number, true},
    {"neverFullRefresh", "永不全刷", M4SettingsControl::Toggle, true},
    {"buttonHintsEnabled", "按钮提示", M4SettingsControl::Toggle, true},
    {"sleepBeforeFullRefresh", "关机前全刷", M4SettingsControl::Toggle, true},
};

constexpr M4SettingsRow kAdvancedChrome[] = {
    {"imageQuality", "图片质量", M4SettingsControl::Choice, true},
    {"iconStyle", "图标风格", M4SettingsControl::Choice, true},
    {"homeIconStyle", "图标选中风格", M4SettingsControl::Choice, true},
    {"uiFontSize", "界面文字", M4SettingsControl::Choice, true},
    {"uiFontFamily", "系统字体", M4SettingsControl::Navigate, true},
};

constexpr M4SettingsRow kAdvancedAnim[] = {
    {"systemAnimationEnabled", "系统动画", M4SettingsControl::Toggle, true},
    {"pageTurnAnimationSteps", "翻页步数", M4SettingsControl::Number, true},
    {"pageTurnAnimationMult", "翻页倍率", M4SettingsControl::Number, true},
    {"pageTurnAnimationTp", "翻页 TP", M4SettingsControl::Number, true},
    {"pageTurnAnimationFrameRate", "翻页帧率", M4SettingsControl::Choice, true},
};

constexpr M4SettingsRow kAdvancedConnect[] = {
    {"wifiAlwaysReselect", "始终重选网络", M4SettingsControl::Toggle, true},
    {"autoSyncTimeOnBoot", "开机同步时间", M4SettingsControl::Toggle, true},
    {"bluetooth", "蓝牙", M4SettingsControl::Navigate, true},
    {"koreader", "KOReader 同步", M4SettingsControl::Navigate, true},
    {"jianguo", "坚果云", M4SettingsControl::Navigate, true},
    {"dataCapsule", "数据胶囊", M4SettingsControl::Navigate, true},
};

constexpr M4SettingsRow kAdvancedSystem[] = {
    {"systemLanguage", "系统语言", M4SettingsControl::Toggle, true},
    {"directTxtRead", "直接打开 TXT", M4SettingsControl::Toggle, true},
};

inline const M4SettingsRow* tableFor(const char* parent, int* count) {
  if (!parent) {
    if (count) *count = 0;
    return nullptr;
  }
  if (std::strcmp(parent, "frontlight") == 0) {
    if (count) *count = (int)(sizeof(kFrontlight) / sizeof(kFrontlight[0]));
    return kFrontlight;
  }
  if (std::strcmp(parent, "keys") == 0) {
    if (count) *count = (int)(sizeof(kKeys) / sizeof(kKeys[0]));
    return kKeys;
  }
  if (std::strcmp(parent, "maintenance") == 0) {
    if (count) *count = (int)(sizeof(kMaintenance) / sizeof(kMaintenance[0]));
    return kMaintenance;
  }
  if (std::strcmp(parent, "advanced") == 0) {
    if (count) *count = (int)(sizeof(kAdvanced) / sizeof(kAdvanced[0]));
    return kAdvanced;
  }
  if (std::strcmp(parent, "advancedDisplay") == 0) {
    if (count) *count = (int)(sizeof(kAdvancedDisplay) / sizeof(kAdvancedDisplay[0]));
    return kAdvancedDisplay;
  }
  if (std::strcmp(parent, "advancedChrome") == 0) {
    if (count) *count = (int)(sizeof(kAdvancedChrome) / sizeof(kAdvancedChrome[0]));
    return kAdvancedChrome;
  }
  if (std::strcmp(parent, "advancedAnim") == 0) {
    if (count) *count = (int)(sizeof(kAdvancedAnim) / sizeof(kAdvancedAnim[0]));
    return kAdvancedAnim;
  }
  if (std::strcmp(parent, "advancedConnect") == 0) {
    if (count) *count = (int)(sizeof(kAdvancedConnect) / sizeof(kAdvancedConnect[0]));
    return kAdvancedConnect;
  }
  if (std::strcmp(parent, "advancedSystem") == 0) {
    if (count) *count = (int)(sizeof(kAdvancedSystem) / sizeof(kAdvancedSystem[0]));
    return kAdvancedSystem;
  }
  if (count) *count = 0;
  return nullptr;
}

inline int linearWindowStart(int selected, int count, int window) {
  if (count <= window) return 0;
  if (selected < 0) selected = 0;
  if (selected >= count) selected = count - 1;
  int start = selected - window + 1;
  if (start < 0) start = 0;
  const int maxStart = count - window;
  if (start > maxStart) start = maxStart;
  return start;
}

}  // namespace m4_settings_root_ui_detail

inline bool m4SettingsUiIsAdvancedPage(const M4SettingsUiState& st) {
  return st.page == M4SettingsPageKind::ChildList && std::strcmp(st.parentKey, "advanced") == 0;
}

inline bool m4SettingsUiIsAdvancedFamily(const M4SettingsUiState& st) {
  if (st.page != M4SettingsPageKind::ChildList) return false;
  if (std::strcmp(st.parentKey, "advanced") == 0) return true;
  return std::strcmp(st.listStack, "advanced") == 0;
}

inline int m4SettingsChildCount(const char* parentKey) {
  int n = 0;
  m4_settings_root_ui_detail::tableFor(parentKey, &n);
  return n;
}

inline const M4SettingsRow* m4SettingsChildAt(const char* parentKey, int index) {
  int n = 0;
  const M4SettingsRow* rows = m4_settings_root_ui_detail::tableFor(parentKey, &n);
  if (!rows || index < 0 || index >= n) return nullptr;
  return &rows[index];
}

inline const char* m4SettingsChildListTitle(const char* parentKey) {
  if (!parentKey || !parentKey[0]) return "设置";
  const M4SettingsRow* rootRow =
      m4SettingsRowByKey(m4SettingsRootCatalog(), kM4SettingsRootCount, parentKey);
  if (rootRow && rootRow->titleZh) return rootRow->titleZh;
  const int n = m4SettingsChildCount("advanced");
  for (int i = 0; i < n; ++i) {
    const M4SettingsRow* row = m4SettingsChildAt("advanced", i);
    if (row && row->key && std::strcmp(row->key, parentKey) == 0 && row->titleZh) return row->titleZh;
  }
  return "设置";
}

inline void m4SettingsFormatWifiValue(const char* ssid, char* out, int outLen) {
  if (!out || outLen <= 0) return;
  const char* src = (ssid && ssid[0]) ? ssid : "未连接";
  m4SettingsCopyKey(out, outLen, src);
}

inline void m4SettingsFormatFrontlightValue(int brightness, int warmth, char* out, int outLen) {
  if (!out || outLen <= 0) return;
  std::snprintf(out, static_cast<size_t>(outLen), "亮度 %d%% · 色温 %d%%", brightness, warmth);
}

inline int m4SettingsWholeRowHit(int x, int y, int originX, int originY, int itemH, int gap, int screenW,
                                int maxRows = 8) {
  if (screenW <= 0 || itemH <= 0) return -1;
  if (x < originX || x >= screenW) return -1;
  if (y < originY) return -1;
  const int stride = itemH + gap;
  if (stride <= 0) return -1;
  // Advanced v5.1 paints 9 rows; every other consumer keeps 8 (Choice theme
  // limit, grouped children, Root uses its own hit fn).
  for (int i = 0; i < maxRows; ++i) {
    const int y0 = originY + i * stride;
    const int y1 = y0 + itemH;
    if (y >= y0 && y < y1) return i;
  }
  return -1;
}

inline bool m4SettingsUiConfirmAccepts(M4ConfirmButton b, bool footerPrimaryOrRow) {
  return m4SettingsDangerAccepts(b, footerPrimaryOrRow);
}

inline int m4SettingsUiVisibleCount(const M4SettingsUiState& st) {
  if (st.page == M4SettingsPageKind::Choice) return st.choiceCount;
  if (st.page == M4SettingsPageKind::ChildList) return m4SettingsChildCount(st.parentKey);
  if (st.page == M4SettingsPageKind::Confirm || st.page == M4SettingsPageKind::Number) return 0;
  return kM4SettingsRootCount;
}

inline const M4SettingsRow* m4SettingsUiVisibleRow(const M4SettingsUiState& st, int index) {
  if (st.page == M4SettingsPageKind::ChildList) return m4SettingsChildAt(st.parentKey, index);
  if (st.page == M4SettingsPageKind::Root) {
    if (index < 0 || index >= kM4SettingsRootCount) return nullptr;
    return &m4SettingsRootCatalog()[index];
  }
  return nullptr;
}

inline void m4SettingsUiEnterRoot(M4SettingsUiState& st) {
  st = M4SettingsUiState{};
  st.page = M4SettingsPageKind::Root;
  st.returnPage = M4SettingsPageKind::Root;
  st.selectedSlot = 0;
  m4SettingsCopyKey(st.selectedKey, (int)sizeof(st.selectedKey), "wifi");
}

inline void m4SettingsUiSelectKey(M4SettingsUiState& st, const char* key) {
  if (!key || !key[0]) return;
  if (st.page == M4SettingsPageKind::ChildList) {
    const int n = m4SettingsChildCount(st.parentKey);
    for (int i = 0; i < n; ++i) {
      const M4SettingsRow* row = m4SettingsChildAt(st.parentKey, i);
      if (row && row->key && std::strcmp(row->key, key) == 0) {
        st.selectedSlot = i;
        st.windowStart = m4_settings_root_ui_detail::linearWindowStart(i, n, 8);
        m4SettingsCopyKey(st.selectedKey, (int)sizeof(st.selectedKey), key);
        return;
      }
    }
  }
  const int i = m4SettingsIndexOfKey(m4SettingsRootCatalog(), kM4SettingsRootCount, key);
  if (i < 0) return;
  st.page = M4SettingsPageKind::Root;
  st.selectedSlot = i;
  st.windowStart = 0;
  m4SettingsCopyKey(st.selectedKey, (int)sizeof(st.selectedKey), key);
}

inline int m4SettingsUiMove(M4SettingsUiState& st, int delta) {
  const int count = m4SettingsUiVisibleCount(st);
  const int next = m4SettingsNavMoveClamp(st.selectedSlot, delta, count);
  st.selectedSlot = next;
  // Advanced v5.1 shows 9 rows per window; Choice pages keep the 8-row
  // theme limit and every other page never exceeds 8 rows.
  const int winCap = 8;
  st.windowStart = m4_settings_root_ui_detail::linearWindowStart(next, count, winCap);
  if (st.page == M4SettingsPageKind::Choice) return next;
  const M4SettingsRow* row = m4SettingsUiVisibleRow(st, next);
  if (row && row->key) m4SettingsCopyKey(st.selectedKey, (int)sizeof(st.selectedKey), row->key);
  return next;
}

inline const char* m4SettingsUiActivateIdentity(const M4SettingsUiState& st) {
  return st.selectedKey;
}

inline int m4SettingsUiStoreIndexForActivation(const char* key, const char* const* store, int n) {
  return m4SettingsStoreIndexForKey(store, n, key);
}

inline const char* m4SettingsUiFooterLabelForSlot(const M4SettingsUiState& st, int slot) {
  if (st.page == M4SettingsPageKind::Choice || st.page == M4SettingsPageKind::Number) {
    return m4SettingsFooterConfirmLabel(M4SettingsControl::Choice);
  }
  if (st.page == M4SettingsPageKind::Confirm) {
    if (std::strcmp(st.selectedKey, "switchBootSlot") == 0) return "切换";
    return m4SettingsFooterConfirmLabel(M4SettingsControl::Confirm);
  }
  if (st.page == M4SettingsPageKind::ChildList) {
    const M4SettingsRow* row = m4SettingsChildAt(st.parentKey, slot);
    if (row) return m4SettingsFooterConfirmLabel(row->control);
  }
  if (slot >= 0 && slot < kM4SettingsRootCount) {
    return m4SettingsFooterConfirmLabel(m4SettingsRootCatalog()[slot].control);
  }
  return m4SettingsFooterConfirmLabel(M4SettingsControl::Navigate);
}

inline const char* m4SettingsUiFooterLabel(const M4SettingsUiState& st) {
  if (st.page == M4SettingsPageKind::Root || st.page == M4SettingsPageKind::ChildList ||
      st.page == M4SettingsPageKind::Choice || st.page == M4SettingsPageKind::Number ||
      st.page == M4SettingsPageKind::Confirm) {
    return m4SettingsUiFooterLabelForSlot(st, st.selectedSlot);
  }
  return m4SettingsFooterConfirmLabel(M4SettingsControl::Navigate);
}

inline M4DirtyUnion m4SettingsUiNoteMove(M4SettingsUiState& st, int oldSlot, int newSlot, int itemH,
                                         int gap, int oldWindowStart = -1) {
  constexpr int kWindowRows = 8;
  // List-local y of screen bottom when originY=112 and screenH=800 (footer 736–800).
  constexpr int kFooterLocalY1 = 800 - 112;
  if (oldWindowStart < 0) oldWindowStart = st.windowStart;
  const int newWindowStart = st.windowStart;
  M4DirtyUnion d{};
  if (oldWindowStart != newWindowStart) {
    d.full = true;
    d.y0 = 0;
    d.y1 = 0;
    st.burstCount = 0;
    return d;
  }
  const int oldVis = oldSlot - oldWindowStart;
  const int newVis = newSlot - newWindowStart;
  if (oldVis < 0 || newVis < 0 || oldVis >= kWindowRows || newVis >= kWindowRows) {
    d.full = true;
    d.y0 = 0;
    d.y1 = 0;
    st.burstCount = 0;
    return d;
  }
  d = m4SettingsDirtyAfterMove(oldVis, newVis, itemH, gap, st.burstCount);
  if (d.full) {
    st.burstCount = 0;
  } else {
    ++st.burstCount;
    const char* oldFooter = m4SettingsUiFooterLabelForSlot(st, oldSlot);
    const char* newFooter = m4SettingsUiFooterLabelForSlot(st, newSlot);
    if (oldFooter && newFooter && std::strcmp(oldFooter, newFooter) != 0) {
      if (d.y1 < kFooterLocalY1) d.y1 = kFooterLocalY1;
    }
  }
  return d;
}

inline void m4SettingsUiOpenChoice(M4SettingsUiState& st, const char* key, int count, int current) {
  st.returnPage = (st.page == M4SettingsPageKind::Choice) ? st.returnPage : st.page;
  st.page = M4SettingsPageKind::Choice;
  m4SettingsCopyKey(st.choiceKey, (int)sizeof(st.choiceKey), key);
  if (st.selectedKey[0] == 0) m4SettingsCopyKey(st.selectedKey, (int)sizeof(st.selectedKey), key);
  st.choiceCount = count < 0 ? 0 : count;
  if (current < 0) current = 0;
  if (st.choiceCount > 0 && current >= st.choiceCount) current = st.choiceCount - 1;
  st.choiceChecked = current;
  st.selectedSlot = current;
  st.windowStart = m4_settings_root_ui_detail::linearWindowStart(current, st.choiceCount, 8);
  st.burstCount = 0;
}

inline bool m4SettingsUiChoiceChecked(const M4SettingsUiState& st, int index) {
  return st.page == M4SettingsPageKind::Choice && index == st.choiceChecked;
}

inline int m4SettingsUiChoiceCommit(M4SettingsUiState& st) {
  const int committed = st.selectedSlot;
  const M4SettingsPageKind back = st.returnPage;
  char restoreKey[32]{};
  m4SettingsCopyKey(restoreKey, (int)sizeof(restoreKey),
                    st.choiceKey[0] ? st.choiceKey : st.selectedKey);
  st.page = (back == M4SettingsPageKind::Choice) ? M4SettingsPageKind::Root : back;
  m4SettingsCopyKey(st.selectedKey, (int)sizeof(st.selectedKey), restoreKey);
  if (st.page == M4SettingsPageKind::Root || st.page == M4SettingsPageKind::ChildList) {
    m4SettingsUiSelectKey(st, restoreKey);
  }
  st.burstCount = 0;
  return committed;
}

inline void m4SettingsUiOpenConfirm(M4SettingsUiState& st, const char* key) {
  st.returnPage = (st.page == M4SettingsPageKind::Confirm) ? st.returnPage : st.page;
  st.page = M4SettingsPageKind::Confirm;
  if (key && key[0]) m4SettingsCopyKey(st.selectedKey, (int)sizeof(st.selectedKey), key);
  st.confirmAccepted = false;
  st.confirmCancelled = false;
  st.bootSlotSwitchRequested = false;
  st.burstCount = 0;
}

inline void m4SettingsUiConfirmCancel(M4SettingsUiState& st) {
  char restoreKey[32]{};
  m4SettingsCopyKey(restoreKey, (int)sizeof(restoreKey), st.selectedKey);
  st.confirmCancelled = true;
  st.confirmAccepted = false;
  st.bootSlotSwitchRequested = false;
  st.page = (st.returnPage == M4SettingsPageKind::Confirm) ? M4SettingsPageKind::Root : st.returnPage;
  st.burstCount = 0;
  if (st.page == M4SettingsPageKind::Root || st.page == M4SettingsPageKind::ChildList) {
    m4SettingsUiSelectKey(st, restoreKey);
  }
}

inline bool m4SettingsUiConfirmDecide(M4SettingsUiState& st, M4ConfirmButton b, bool footerPrimaryOrRow) {
  if (st.page != M4SettingsPageKind::Confirm) return false;
  if (b == M4ConfirmButton::Back) {
    m4SettingsUiConfirmCancel(st);
    return false;
  }
  if (!m4SettingsUiConfirmAccepts(b, footerPrimaryOrRow)) return false;
  st.confirmAccepted = true;
  st.confirmCancelled = false;
  if (std::strcmp(st.selectedKey, "switchBootSlot") == 0) {
    st.bootSlotSwitchRequested = true;
  }
  return true;
}

// Root grouped-layout geometry. Visual metadata only: flat catalog order,
// selection identity, window math, and footer/input contracts never see groups.
// Root never scrolls (8 rows == window), so bands are absolute.
constexpr int kM4SettingsRootOriginY = 112;
constexpr int kM4SettingsRootRowH = 58;
constexpr int kM4SettingsL2OriginY = 130;
constexpr int kM4SettingsL2ItemH = 58;
constexpr int kM4SettingsL2Gap = 0;
// Advanced: 9-row window at the same 58px pitch as Root/L2, origin y130
// so the list card never encloses the page title/hairline.
constexpr int kM4SettingsAdvancedOriginY = 130;
constexpr int kM4SettingsAdvancedItemH = 58;
constexpr int kM4SettingsAdvancedGap = 0;
// Advanced v5.1 section grammar (layout-v5.1-advanced): real group intervals
// shared by paint, touch, and tests. A 42px gap plus the new group's title
// (baseline = gapTop + 32) precedes every within-window group boundary, so
// the first screen lands G1 exactly on card 428..636. List-engine math
// (order/count/selection/windowStart) never sees the gap; only paint Y and
// the hit table below do.
constexpr int kM4SettingsAdvancedGroupStarts[] = {0, 5, 9, 14, 20};
constexpr int kM4SettingsAdvancedGroupCount = 5;
constexpr int kM4SettingsAdvancedSectionGap = 24;
constexpr int kM4SettingsAdvancedSectionTitleBaselineDy = 32;  // from gap top

inline int m4SettingsAdvancedGroupOf(int flatIndex) {
  int g = 0;
  while (g + 1 < kM4SettingsAdvancedGroupCount && flatIndex >= kM4SettingsAdvancedGroupStarts[g + 1]) {
    ++g;
  }
  return g;
}

inline bool m4SettingsAdvancedSlotStartsGroup(int windowStart, int slot) {
  if (slot <= 0) return false;
  return m4SettingsAdvancedGroupOf(windowStart + slot) !=
         m4SettingsAdvancedGroupOf(windowStart + slot - 1);
}

inline int m4SettingsAdvancedRowY(int windowStart, int slot) {
  (void)windowStart;
  return kM4SettingsAdvancedOriginY + slot * (kM4SettingsAdvancedItemH + kM4SettingsAdvancedGap);
}

inline int m4SettingsAdvancedRowHit(int x, int y, int windowStart) {
  if (x < 0 || x >= 480) return -1;
  for (int i = 0; i < 9; ++i) {
    const int y0 = m4SettingsAdvancedRowY(windowStart, i);
    if (y >= y0 && y < y0 + kM4SettingsAdvancedItemH) return i;
  }
  return -1;
}
// Small grouped child pages (frontlight/keys/maintenance) carry the same header
// one label lower so their single static group (label + card) fits; rows keep
// L2 pitch and the flat ChildList order/count/identity math never changes.
constexpr int kM4SettingsGroupedChildOriginY = 130;

inline bool m4SettingsUiIsGroupedChildPage(const M4SettingsUiState& st) {
  if (st.page != M4SettingsPageKind::ChildList) return false;
  return std::strcmp(st.parentKey, "frontlight") == 0 || std::strcmp(st.parentKey, "keys") == 0 ||
         std::strcmp(st.parentKey, "maintenance") == 0 || m4SettingsUiIsAdvancedFamily(st);
}

inline int m4SettingsUiContentOriginY(const M4SettingsUiState& st) {
  if (st.page == M4SettingsPageKind::Root) return kM4SettingsRootOriginY;
  if (m4SettingsUiIsGroupedChildPage(st)) return kM4SettingsGroupedChildOriginY;
  if (m4SettingsUiIsAdvancedPage(st)) return kM4SettingsAdvancedOriginY;
  return kM4SettingsL2OriginY;
}

// Row pitch for paint-note dirty, whole-row touch, and thumb track math.
// Advanced v4 only; grouped children, Choice, and any other L2 consumer keep
// the 72/4 pitch untouched.
inline int m4SettingsUiListItemH(const M4SettingsUiState& st) {
  return m4SettingsUiIsAdvancedPage(st) ? kM4SettingsAdvancedItemH : kM4SettingsL2ItemH;
}

inline int m4SettingsUiListItemGap(const M4SettingsUiState& st) {
  return m4SettingsUiIsAdvancedPage(st) ? kM4SettingsAdvancedGap : kM4SettingsL2Gap;
}

inline int m4SettingsRootRowY0(int flatSlot) {
  // Absolute row tops == theme repeat origins (v5.1 58px rows, no intra-card gap).
  constexpr int kY0[kM4SettingsRootCount] = {130, 230, 288, 388, 446, 504, 604, 662};
  if (flatSlot < 0 || flatSlot >= kM4SettingsRootCount) return -1;
  return kY0[flatSlot];
}

inline int m4SettingsRootRowHit(int x, int y, int screenW) {
  if (screenW <= 0 || x < 0 || x >= screenW) return -1;
  for (int i = 0; i < kM4SettingsRootCount; ++i) {
    const int y0 = m4SettingsRootRowY0(i);
    if (y >= y0 && y < y0 + kM4SettingsRootRowH) return i;
  }
  return -1;
}

// Root dirty union in list-local coords, mirroring m4SettingsDirtyAfterMove.
inline M4DirtyUnion m4SettingsRootDirtyAfterMove(int oldSlot, int newSlot, int burstCount) {
  M4DirtyUnion out{};
  if (burstCount >= kM4PartialBurst) {
    out.full = true;
    out.y0 = 0;
    out.y1 = 0;
    return out;
  }
  const int a = m4SettingsRootRowY0(oldSlot);
  const int b = m4SettingsRootRowY0(newSlot);
  if (a < 0 || b < 0) {
    out.full = true;
    out.y0 = 0;
    out.y1 = 0;
    return out;
  }
  out.full = false;
  out.y0 = (a < b ? a : b) - kM4SettingsRootOriginY;
  out.y1 = (a < b ? b : a) + kM4SettingsRootRowH - kM4SettingsRootOriginY;
  return out;
}

inline M4DirtyUnion m4SettingsUiNoteMoveRoot(M4SettingsUiState& st, int oldSlot, int newSlot) {
  M4DirtyUnion d = m4SettingsRootDirtyAfterMove(oldSlot, newSlot, st.burstCount);
  if (d.full) {
    st.burstCount = 0;
  } else {
    ++st.burstCount;
    constexpr int kRootFooterLocalY1 = 800 - kM4SettingsRootOriginY;
    const char* oldFooter = m4SettingsUiFooterLabelForSlot(st, oldSlot);
    const char* newFooter = m4SettingsUiFooterLabelForSlot(st, newSlot);
    if (oldFooter && newFooter && std::strcmp(oldFooter, newFooter) != 0) {
      if (d.y1 < kRootFooterLocalY1) d.y1 = kRootFooterLocalY1;
    }
  }
  return d;
}

inline void m4SettingsUiOpenChildList(M4SettingsUiState& st, const char* parentKey, bool push = true) {
  if (push && st.page == M4SettingsPageKind::ChildList && st.parentKey[0] && parentKey &&
      std::strcmp(st.parentKey, parentKey) != 0) {
    m4SettingsCopyKey(st.listStack, (int)sizeof(st.listStack), st.parentKey);
  } else if (!push) {
    st.listStack[0] = '\0';
  }
  st.returnPage = M4SettingsPageKind::Root;
  st.page = M4SettingsPageKind::ChildList;
  m4SettingsCopyKey(st.parentKey, (int)sizeof(st.parentKey), parentKey);
  st.selectedSlot = 0;
  st.windowStart = 0;
  st.burstCount = 0;
  const M4SettingsRow* first = m4SettingsChildAt(parentKey, 0);
  m4SettingsCopyKey(st.selectedKey, (int)sizeof(st.selectedKey), first && first->key ? first->key : "");
}

inline bool m4SettingsUiBackFromChildList(M4SettingsUiState& st) {
  if (st.page != M4SettingsPageKind::ChildList) return false;
  if (st.listStack[0]) {
    char here[32]{};
    m4SettingsCopyKey(here, (int)sizeof(here), st.parentKey);
    char up[32]{};
    m4SettingsCopyKey(up, (int)sizeof(up), st.listStack);
    m4SettingsUiOpenChildList(st, up, false);
    m4SettingsUiSelectKey(st, here);
    return true;
  }
  char parent[32]{};
  m4SettingsCopyKey(parent, (int)sizeof(parent), st.parentKey);
  m4SettingsUiEnterRoot(st);
  m4SettingsUiSelectKey(st, parent);
  return true;
}

inline void m4SettingsUiPushEditor(M4SettingsUiState& st) {
  st.returnPage = st.page;
  st.page = M4SettingsPageKind::Number;
  st.editorCommitted = false;
}

inline void m4SettingsUiPopEditor(M4SettingsUiState& st, bool committed) {
  st.editorCommitted = committed;
  st.page = st.returnPage;
  if (st.page == M4SettingsPageKind::Number) st.page = M4SettingsPageKind::ChildList;
  st.burstCount = 0;
}
