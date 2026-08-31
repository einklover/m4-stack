#pragma once

#include <cstdint>
#include <cstring>
#include <cstddef>

// Settings Hub policy is deliberately independent of SettingsActivity and
// Arduino headers. It is the single source for device-side bucket order and
// for the Hub/category touch geometry. Ported from adaptation branch
// feature/critical-ui-adaptation and extended with flatten + window APIs
// per 2026-08-31-settings-l2-scene spec §7.

enum class SettingsHubCard : uint8_t {
  DisplayReading = 0,
  KeysOperations = 1,
  NetworkSync = 2,
  SystemMaintenance = 3,
  // Compatibility aliases for the original RED contract spelling.
  KeysOps = KeysOperations,
  SystemMaint = SystemMaintenance,
};

enum class SettingsPane : uint8_t { Hub, Category };

struct SettingsNavState {
  SettingsPane pane = SettingsPane::Hub;
  SettingsHubCard hub = SettingsHubCard::DisplayReading;
  int selectedRow = 0;   // setting-row index inside current card (0-based, skips sections)
  int windowStart = 0;   // flattened window origin (0..flatCount-8)
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

struct SettingsHubRow {
  const char* key = "";
  uint8_t section = 0;
};

inline const char* settingsHubCardTitleZh(SettingsHubCard card) {
  switch (card) {
    case SettingsHubCard::DisplayReading: return "显示与阅读";
    case SettingsHubCard::KeysOperations: return "按键与操作";
    case SettingsHubCard::NetworkSync: return "网络与同步";
    case SettingsHubCard::SystemMaintenance: return "系统与维护";
    default: return "";
  }
}

namespace settings_hub_policy_detail {

struct CatalogRow {
  SettingsHubRow row;
  bool m4Only;
};

inline constexpr CatalogRow kDisplayRows[] = {
    {{"sleepScreen", 0}, false},
    {{"statusBar", 0}, false},
    {{"hideBatteryPercentage", 0}, false},
    {{"refreshFrequency", 1}, false},
    {{"neverFullRefresh", 1}, false},
    {{"buttonHintsEnabled", 0}, false},
    {{"frontlightBrightness", 2}, true},
    {{"frontlightWarmth", 2}, true},
    {{"sleepBeforeFullRefresh", 1}, false},
    {{"imageQuality", 0}, false},
    {{"iconStyle", 0}, false},
    {{"homeIconStyle", 0}, false},
    {{"uiFontSize", 0}, true},
    {{"readerLayout", 3}, false},
    {{"systemAnimationEnabled", 4}, true},
    {{"pageTurnAnimationSteps", 4}, true},
    {{"pageTurnAnimationMult", 4}, true},
    {{"pageTurnAnimationTp", 4}, true},
    {{"pageTurnAnimationFrameRate", 4}, true},
};

inline constexpr SettingsHubRow kKeysRows[] = {
    {"remapButtons", 0},
    {"sideButtonLayout", 0},
    {"shortPwrBtn", 0},
    {"longPressChapterSkip", 0},
    {"longPressBoot", 0},
    {"libraryLongPressMenu", 0},
};

inline constexpr SettingsHubRow kNetworkRows[] = {
    {"wifiAlwaysReselect", 0},
    {"autoSyncTimeOnBoot", 0},
    {"bluetooth", 1},
    {"koreader", 1},
    {"jianguo", 1},
    {"dataCapsule", 1},
};

inline constexpr CatalogRow kSystemRows[] = {
    {{"systemLanguage", 0}, false},
    {{"sleepTimeout", 0}, false},
    {{"directTxtRead", 0}, false},
    {{"clearCache", 1}, false},
    {{"resetSettings", 1}, false},
    {{"developerOptions", 1}, true},
    {{"switchBootSlot", 1}, true},
};

template <size_t N>
inline int availableCount(const CatalogRow (&rows)[N], bool m4Build) {
  int count = 0;
  for (const auto& entry : rows) {
    if (m4Build || !entry.m4Only) ++count;
  }
  return count;
}

template <size_t N>
inline SettingsHubRow availableAt(const CatalogRow (&rows)[N], int index, bool m4Build) {
  if (index < 0) return {};
  int visible = 0;
  for (const auto& entry : rows) {
    if (!m4Build && entry.m4Only) continue;
    if (visible++ == index) return entry.row;
  }
  return {};
}

inline int cardIndex(SettingsHubCard card) {
  const int index = static_cast<int>(card);
  return index >= 0 && index < kSettingsHubCardCount ? index : 0;
}

// Section title helpers for flattened L2
inline const char* displaySectionTitle(uint8_t section) {
  switch (section) {
    case 0: return "界面";
    case 1: return "墨水屏刷新";
    case 2: return "前光";
    case 3: return "阅读排版";
    case 4: return "翻页动画";
    default: return "";
  }
}
inline const char* networkSectionTitle(uint8_t section) {
  switch (section) {
    case 0: return "网络";
    case 1: return "同步入口";
    default: return "";
  }
}
inline const char* systemSectionTitle(uint8_t section) {
  switch (section) {
    case 0: return "系统";
    case 1: return "维护";
    default: return "";
  }
}

}  // namespace settings_hub_policy_detail

// ---------------------------------------------------------------------------
// Hub row APIs (port from adaptation)
// ---------------------------------------------------------------------------

inline int settingsHubRowCount(SettingsHubCard card, bool m4Build, bool /*x3Build*/) {
  using namespace settings_hub_policy_detail;
  switch (cardIndex(card)) {
    case 0: return availableCount(kDisplayRows, m4Build);
    case 1: return static_cast<int>(sizeof(kKeysRows) / sizeof(kKeysRows[0]));
    case 2: return static_cast<int>(sizeof(kNetworkRows) / sizeof(kNetworkRows[0]));
    case 3: return availableCount(kSystemRows, m4Build);
    default: return 0;
  }
}

inline SettingsHubRow settingsHubRowAt(SettingsHubCard card, int index, bool m4Build, bool /*x3Build*/) {
  using namespace settings_hub_policy_detail;
  switch (cardIndex(card)) {
    case 0: return availableAt(kDisplayRows, index, m4Build);
    case 1:
      return index >= 0 && index < static_cast<int>(sizeof(kKeysRows) / sizeof(kKeysRows[0])) ? kKeysRows[index] : SettingsHubRow{};
    case 2:
      return index >= 0 && index < static_cast<int>(sizeof(kNetworkRows) / sizeof(kNetworkRows[0])) ? kNetworkRows[index] : SettingsHubRow{};
    case 3: return availableAt(kSystemRows, index, m4Build);
    default: return {};
  }
}

inline bool settingsHubContainsKey(SettingsHubCard card, const char* key, bool m4Build, bool x3Build) {
  if (!key) return false;
  const int count = settingsHubRowCount(card, m4Build, x3Build);
  for (int i = 0; i < count; ++i) {
    const auto row = settingsHubRowAt(card, i, m4Build, x3Build);
    if (row.key && std::strcmp(row.key, key) == 0) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Flatten + window APIs (new per spec §7)
// ---------------------------------------------------------------------------

inline int settingsFlatCount(SettingsHubCard card, bool m4Build) {
  using namespace settings_hub_policy_detail;
  const int idx = cardIndex(card);
  if (idx == 0) { // DisplayReading
    // 5 sections, but frontlight and pageTurn omitted on non-M4 if empty
    int total = 0;
    for (uint8_t sec = 0; sec < 5; ++sec) {
      int secCount = 0;
      for (const auto& e : kDisplayRows) {
        if (e.row.section != sec) continue;
        if (!m4Build && e.m4Only) continue;
        ++secCount;
      }
      if (secCount == 0) continue;
      // section title + rows
      total += 1 + secCount;
    }
    return total;
  } else if (idx == 1) { // KeysOperations : single group, no title
    return settingsHubRowCount(card, m4Build, false);
  } else if (idx == 2) { // NetworkSync : 2 sections
    int total = 0;
    for (uint8_t sec = 0; sec < 2; ++sec) {
      int secCount = 0;
      for (const auto& r : kNetworkRows) if (r.section == sec) ++secCount;
      if (secCount == 0) continue;
      total += 1 + secCount;
    }
    return total;
  } else if (idx == 3) { // SystemMaintenance
    int total = 0;
    for (uint8_t sec = 0; sec < 2; ++sec) {
      int secCount = 0;
      for (const auto& e : kSystemRows) {
        if (e.row.section != sec) continue;
        if (!m4Build && e.m4Only) continue;
        ++secCount;
      }
      if (secCount == 0) continue;
      total += 1 + secCount;
    }
    return total;
  }
  return 0;
}

inline SettingsFlatRow settingsFlatAt(SettingsHubCard card, int flatIndex, bool m4Build) {
  using namespace settings_hub_policy_detail;
  if (flatIndex < 0) return {};
  const int idx = cardIndex(card);
  int cursor = 0;
  if (idx == 0) {
    for (uint8_t sec = 0; sec < 5; ++sec) {
      // collect rows for this section in insertion order
      int secCount = 0;
      for (const auto& e : kDisplayRows) if (e.row.section == sec && (m4Build || !e.m4Only)) ++secCount;
      if (secCount == 0) continue;
      if (cursor == flatIndex) {
        SettingsFlatRow r{};
        r.kind = SettingsFlatKind::Section;
        r.section = sec;
        r.key = "";
        r.titleZh = displaySectionTitle(sec);
        return r;
      }
      ++cursor;
      for (const auto& e : kDisplayRows) {
        if (e.row.section != sec) continue;
        if (!m4Build && e.m4Only) continue;
        if (cursor == flatIndex) {
          SettingsFlatRow r{};
          r.kind = SettingsFlatKind::Setting;
          r.section = sec;
          r.key = e.row.key;
          r.titleZh = e.row.key; // for product, titleZh mirrors key; i18n resolved elsewhere
          return r;
        }
        ++cursor;
      }
    }
  } else if (idx == 1) {
    // KeysOperations: no section rows
    const int rc = settingsHubRowCount(card, m4Build, false);
    if (flatIndex < rc) {
      auto row = settingsHubRowAt(card, flatIndex, m4Build, false);
      SettingsFlatRow r{};
      r.kind = SettingsFlatKind::Setting;
      r.section = row.section;
      r.key = row.key;
      r.titleZh = row.key;
      return r;
    }
  } else if (idx == 2) {
    for (uint8_t sec = 0; sec < 2; ++sec) {
      int secCount = 0;
      for (const auto& r : kNetworkRows) if (r.section == sec) ++secCount;
      if (secCount == 0) continue;
      if (cursor == flatIndex) {
        SettingsFlatRow r{};
        r.kind = SettingsFlatKind::Section;
        r.section = sec;
        r.key = "";
        r.titleZh = networkSectionTitle(sec);
        return r;
      }
      ++cursor;
      for (const auto& r : kNetworkRows) {
        if (r.section != sec) continue;
        if (cursor == flatIndex) {
          SettingsFlatRow out{};
          out.kind = SettingsFlatKind::Setting;
          out.section = sec;
          out.key = r.key;
          out.titleZh = r.key;
          return out;
        }
        ++cursor;
      }
    }
  } else if (idx == 3) {
    for (uint8_t sec = 0; sec < 2; ++sec) {
      int secCount = 0;
      for (const auto& e : kSystemRows) if (e.row.section == sec && (m4Build || !e.m4Only)) ++secCount;
      if (secCount == 0) continue;
      if (cursor == flatIndex) {
        SettingsFlatRow r{};
        r.kind = SettingsFlatKind::Section;
        r.section = sec;
        r.key = "";
        r.titleZh = systemSectionTitle(sec);
        return r;
      }
      ++cursor;
      for (const auto& e : kSystemRows) {
        if (e.row.section != sec) continue;
        if (!m4Build && e.m4Only) continue;
        if (cursor == flatIndex) {
          SettingsFlatRow r{};
          r.kind = SettingsFlatKind::Setting;
          r.section = sec;
          r.key = e.row.key;
          r.titleZh = e.row.key;
          return r;
        }
        ++cursor;
      }
    }
  }
  return {};
}

inline int settingsFlatIndexOfSetting(SettingsHubCard card, int settingIndex, bool m4Build) {
  if (settingIndex < 0) return -1;
  const int rc = settingsHubRowCount(card, m4Build, false);
  if (settingIndex >= rc) return -1;
  const int flatCount = settingsFlatCount(card, m4Build);
  int seenSettings = 0;
  for (int f = 0; f < flatCount; ++f) {
    auto row = settingsFlatAt(card, f, m4Build);
    if (row.kind == SettingsFlatKind::Setting) {
      if (seenSettings == settingIndex) return f;
      ++seenSettings;
    }
  }
  return -1;
}

inline int settingsWindowStart(int flatIndex, int flatCount, int window = kSettingsL2Window) {
  if (window <= 0) return 0;
  if (flatCount <= window) return 0;
  if (flatIndex < 0) flatIndex = 0;
  if (flatIndex >= flatCount) flatIndex = flatCount - 1;
  int start = flatIndex - window + 1;
  if (start < 0) start = 0;
  int maxStart = flatCount - window;
  if (start > maxStart) start = maxStart;
  // Ensure window still contains flatIndex if earlier clamping moved it out
  if (flatIndex < start) start = flatIndex;
  if (flatIndex >= start + window) start = flatIndex - window + 1;
  if (start < 0) start = 0;
  if (start > maxStart) start = maxStart;
  return start;
}

// ---------------------------------------------------------------------------
// Nav state helpers
// ---------------------------------------------------------------------------

inline SettingsNavState settingsNavOpenCard(SettingsNavState state, SettingsHubCard card) {
  const int index = settings_hub_policy_detail::cardIndex(card);
  state.pane = SettingsPane::Category;
  state.hub = static_cast<SettingsHubCard>(index);
  state.selectedRow = 0;
  // sync window to first row
  const int flatCount = settingsFlatCount(state.hub, true);
  const int flatIndex = settingsFlatIndexOfSetting(state.hub, state.selectedRow, true);
  state.windowStart = settingsWindowStart(flatIndex >=0 ? flatIndex : 0, flatCount, kSettingsL2Window);
  return state;
}

inline SettingsNavState settingsNavBack(SettingsNavState state) {
  if (state.pane == SettingsPane::Category) state.pane = SettingsPane::Hub;
  return state;
}

inline SettingsNavState settingsNavMoveHub(SettingsNavState state, int delta) {
  if (state.pane != SettingsPane::Hub || delta == 0) return state;
  int next = static_cast<int>(state.hub) + delta;
  next %= kSettingsHubCardCount;
  if (next < 0) next += kSettingsHubCardCount;
  state.hub = static_cast<SettingsHubCard>(next);
  return state;
}

inline SettingsNavState settingsNavMoveRow(SettingsNavState state, int delta, int settingCount) {
  if (state.pane != SettingsPane::Category || settingCount <= 0 || delta == 0) return state;
  int next = state.selectedRow + delta;
  next %= settingCount;
  if (next < 0) next += settingCount;
  state.selectedRow = next;
  // sync window after move (use M4 assumption; caller may re-sync with actual build flag)
  return state;
}

inline SettingsNavState settingsNavSyncWindow(SettingsNavState state, SettingsHubCard card, bool m4Build) {
  if (state.pane != SettingsPane::Category) return state;
  const int flatCount = settingsFlatCount(card, m4Build);
  const int flatIndex = settingsFlatIndexOfSetting(card, state.selectedRow, m4Build);
  if (flatCount <= 0 || flatIndex < 0) {
    state.windowStart = 0;
    return state;
  }
  state.windowStart = settingsWindowStart(flatIndex, flatCount, kSettingsL2Window);
  return state;
}

inline SettingsNavState settingsNavActivateConfirm(SettingsNavState state) {
  if (state.pane == SettingsPane::Hub) {
    state.pane = SettingsPane::Category;
    state.selectedRow = 0;
    state.windowStart = 0;
    const int flatCount = settingsFlatCount(state.hub, true);
    const int flatIndex = settingsFlatIndexOfSetting(state.hub, state.selectedRow, true);
    state.windowStart = settingsWindowStart(flatIndex >=0 ? flatIndex : 0, flatCount, kSettingsL2Window);
  }
  return state;
}

inline SettingsNavState settingsNavReturnFromPicker(SettingsNavState state) {
  state.pane = SettingsPane::Category;
  return state;
}

// ---------------------------------------------------------------------------
// Layout / hit geometry (port from adaptation)
// ---------------------------------------------------------------------------

struct SettingsHubLayout {
  struct R {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool contains(int px, int py) const {
      return px >= x && px < x + width && py >= y && py < y + height;
    }
  };
  R cards[kSettingsHubCardCount];
};

inline SettingsHubLayout makeSettingsHubLayout(int screenWidth, int screenHeight, int contentTop, int footerHeight,
                                               int sidePad, int gap) {
  SettingsHubLayout layout;
  if (screenWidth <= 0 || screenHeight <= 0 || footerHeight < 0 || sidePad < 0 || gap < 0) return layout;
  const int contentBottom = screenHeight - footerHeight;
  const int availableHeight = contentBottom - contentTop;
  const int cardHeight = (availableHeight - 3 * gap) / kSettingsHubCardCount;
  const int cardWidth = screenWidth - 2 * sidePad;
  if (cardWidth <= 0 || cardHeight <= 0) return layout;
  for (int i = 0; i < kSettingsHubCardCount; ++i) {
    layout.cards[i] = {sidePad, contentTop + i * (cardHeight + gap), cardWidth, cardHeight};
  }
  return layout;
}

inline int settingsHubCardFromPoint(const SettingsHubLayout& layout, int x, int y) {
  for (int i = 0; i < kSettingsHubCardCount; ++i) {
    if (layout.cards[i].contains(x, y)) return i;
  }
  return -1;
}

inline int settingsCategoryRowFromPoint(int y, int listTop, int listHeight, int rowStep, int rowCount,
                                        int selectedRow) {
  if (rowCount <= 0 || listHeight <= 0 || rowStep <= 0 || y < listTop || y >= listTop + listHeight) return -1;
  const int pageItems = listHeight / rowStep > 0 ? listHeight / rowStep : 1;
  int anchor = selectedRow;
  if (anchor < 0) anchor = 0;
  if (anchor >= rowCount) anchor = rowCount - 1;
  const int pageStart = (anchor / pageItems) * pageItems;
  const int row = (y - listTop) / rowStep;
  const int index = pageStart + row;
  return row >= 0 && row < pageItems && index < rowCount ? index : -1;
}
