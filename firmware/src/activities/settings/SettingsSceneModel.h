#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "activities/settings/SettingsHubPolicy.h"
#include "ui/scene/UiSceneRuntime.h"
#include "ui/scene/UiSceneTypes.h"
#include "ui/scene/UiStateStore.h"

namespace SettingsScene {

// Numeric ABI — must match themes/murphy-settings/{hub,l2}.json bindings/actions
// and stay stable for the generated packages. Keep IDs bounded 1..254
// and non-colliding with reserved common/legacy IDs (1,2,10-15,20,21,30-35).
constexpr UiScene::BindingId kBindingSystemBattery = 1;
constexpr UiScene::BindingId kBindingPageTitle = 70;
constexpr UiScene::BindingId kBindingHubCards = 71;
constexpr UiScene::BindingId kBindingPageRows = 72;
constexpr UiScene::BindingId kBindingItemTitle = 32; // common
constexpr UiScene::BindingId kBindingItemValue = 74;
constexpr UiScene::BindingId kBindingItemSelected = 75;
constexpr UiScene::BindingId kBindingItemIsSection = 76;
constexpr UiScene::BindingId kBindingItemIsRow = 77;
constexpr UiScene::BindingId kBindingItemId = 30; // common

constexpr UiScene::ActionId kActionOpenHubCard = 40;
constexpr UiScene::ActionId kActionActivateSetting = 41;

constexpr std::size_t kMaxWindowRows = 8;
constexpr std::size_t kMaxTextBytes = 2048;
constexpr std::size_t kMaxActionArgumentBytes = 64;
constexpr uint8_t kInvalidItemIndex = 0xFF;

struct SettingsTextRef {
  uint16_t offset = 0;
  uint16_t length = 0;
};

struct SettingsWindowRow {
  SettingsTextRef id{};
  SettingsTextRef title{};
  SettingsTextRef value{};
  bool isSection = false;
  bool isRow = false;
  bool selected = false;
};

struct SettingsSnapshot {
  uint32_t revision = 0;
  UiScene::DataState state = UiScene::DataState::Loading;
  uint8_t errorCode = 0;
  bool stale = false;
  int32_t battery = 0;
  SettingsPane pane = SettingsPane::Hub;
  SettingsHubCard hub = SettingsHubCard::DisplayReading;
  SettingsTextRef pageTitle{};
  uint8_t hubCount = 0;
  SettingsTextRef hubIds[kSettingsHubCardCount]{};
  SettingsTextRef hubTitles[kSettingsHubCardCount]{};
  uint8_t windowCount = 0;
  SettingsWindowRow window[kMaxWindowRows]{};
  uint16_t textUsed = 0;
  char text[kMaxTextBytes]{};

  UiSceneRuntime::TextView textView(SettingsTextRef ref) const {
    if (ref.length == 0 || ref.offset > textUsed || ref.length > textUsed - ref.offset) return {};
    return UiSceneRuntime::TextView::fromRam(text + ref.offset, ref.length);
  }
};

struct SettingsActionTarget {
  UiScene::ActionId action = UiScene::kInvalidActionId;
  uint8_t itemIndex = kInvalidItemIndex;
  uint8_t argumentLength = 0;
  char argument[kMaxActionArgumentBytes]{};

  UiSceneRuntime::TextView argumentView() const {
    return argumentLength == 0 ? UiSceneRuntime::TextView{} : UiSceneRuntime::TextView::fromRam(argument, argumentLength);
  }
};

class SettingsSceneModel final {
 public:
  SettingsSceneModel();
  void begin(UiScene::DataState state);
  bool publish();
  bool publishLoading();
  bool publishEmpty(uint8_t errorCode = 0);
  bool publishError(uint8_t errorCode);
  bool publishStale();

  bool setBattery(int32_t percent);
  bool setPane(SettingsPane pane);
  bool setHub(SettingsHubCard hub);
  bool setPageTitle(const char* title);
  bool setHubCard(uint8_t index, const char* id, const char* title);
  bool setWindowRow(uint8_t index, const char* id, const char* title, const char* value, bool isSection, bool selected);
  bool clearWindow();
  bool populateHubFromPolicy();
  bool populateWindowFromPolicy(SettingsHubCard card, const SettingsNavState& nav, bool m4Build);

  bool copyLatest(SettingsSnapshot& out) const;
  bool hasPublishedSnapshot() const;
  uint32_t latestRevision() const;

  static UiSceneRuntime::SceneBindingSource bindingSource(const SettingsSnapshot& snapshot);
  static bool actionTarget(const SettingsSnapshot& snapshot, UiScene::ActionId action,
                           const UiSceneRuntime::SceneItemContext* item, SettingsActionTarget* out);
  bool actionTarget(UiScene::ActionId action, const UiSceneRuntime::SceneItemContext* item, SettingsActionTarget* out) const;

 private:
  static constexpr std::size_t kMaxStringProbe = kMaxTextBytes + 1;
  SettingsSnapshot draft_{};
  mutable UiStateStore<SettingsSnapshot> store_;
  std::atomic<bool> published_{false};
  uint32_t nextRevision_{1};

  static SettingsSnapshot initialSnapshot();
  static std::size_t boundedLength(const char* value);
  bool canAppend(const char* const* values, std::size_t count) const;
  SettingsTextRef appendText(const char* value);
  static bool resolve(const void* user, UiScene::BindingId binding, const UiSceneRuntime::SceneItemContext* item, UiSceneRuntime::ResolvedValue* out);
  static uint8_t count(const void* user, UiScene::BindingId source);
};

inline SettingsSnapshot SettingsSceneModel::initialSnapshot() {
  SettingsSnapshot s{};
  s.state = UiScene::DataState::Loading;
  s.hubCount = kSettingsHubCardCount;
  s.windowCount = static_cast<uint8_t>(kMaxWindowRows);
  return s;
}

inline SettingsSceneModel::SettingsSceneModel() : store_(initialSnapshot()) { draft_ = initialSnapshot(); }

inline std::size_t SettingsSceneModel::boundedLength(const char* value) {
  if (!value) return 0;
  std::size_t len = 0;
  while (len < kMaxStringProbe && value[len] != '\0') ++len;
  return len;
}

inline bool SettingsSceneModel::canAppend(const char* const* values, std::size_t count) const {
  std::size_t needed = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t len = boundedLength(values[i]);
    if (len >= kMaxStringProbe) return false;
    needed += len;
  }
  return needed <= kMaxTextBytes - draft_.textUsed;
}

inline SettingsTextRef SettingsSceneModel::appendText(const char* value) {
  const std::size_t len = boundedLength(value);
  if (len >= kMaxStringProbe || len > kMaxTextBytes - draft_.textUsed) return {};
  const SettingsTextRef ref{static_cast<uint16_t>(draft_.textUsed), static_cast<uint16_t>(len)};
  if (len != 0) std::memcpy(draft_.text + draft_.textUsed, value, len);
  draft_.textUsed = static_cast<uint16_t>(draft_.textUsed + len);
  return ref;
}

inline void SettingsSceneModel::begin(UiScene::DataState state) {
  draft_ = SettingsSnapshot{};
  draft_.state = state;
  draft_.hubCount = kSettingsHubCardCount;
  draft_.windowCount = static_cast<uint8_t>(kMaxWindowRows);
}

inline bool SettingsSceneModel::publish() {
  draft_.revision = nextRevision_++;
  if (!store_.tryPublish(draft_)) return false;
  published_.store(true, std::memory_order_release);
  return true;
}

inline bool SettingsSceneModel::publishLoading() { begin(UiScene::DataState::Loading); return publish(); }
inline bool SettingsSceneModel::publishEmpty(uint8_t errorCode) { begin(UiScene::DataState::Empty); draft_.errorCode = errorCode; return publish(); }
inline bool SettingsSceneModel::publishError(uint8_t errorCode) { begin(UiScene::DataState::Error); draft_.errorCode = errorCode; return publish(); }
inline bool SettingsSceneModel::publishStale() {
  SettingsSnapshot snap{};
  if (!copyLatest(snap)) return false;
  snap.state = UiScene::DataState::Stale;
  snap.stale = true;
  snap.revision = nextRevision_++;
  if (!store_.tryPublish(snap)) return false;
  published_.store(true, std::memory_order_release);
  return true;
}

inline bool SettingsSceneModel::setBattery(int32_t percent) { draft_.battery = percent; return true; }
inline bool SettingsSceneModel::setPane(SettingsPane pane) { draft_.pane = pane; return true; }
inline bool SettingsSceneModel::setHub(SettingsHubCard hub) { draft_.hub = hub; return true; }

inline bool SettingsSceneModel::setPageTitle(const char* title) {
  if (!title) title = "";
  const char* vals[] = {title};
  if (!canAppend(vals, 1)) return false;
  draft_.pageTitle = appendText(title);
  return true;
}

inline bool SettingsSceneModel::setHubCard(uint8_t index, const char* id, const char* title) {
  if (index >= kSettingsHubCardCount) return false;
  const char* vals[] = {id, title};
  if (!canAppend(vals, 2)) return false;
  draft_.hubIds[index] = appendText(id);
  draft_.hubTitles[index] = appendText(title);
  if (draft_.hubCount < kSettingsHubCardCount) draft_.hubCount = kSettingsHubCardCount;
  return true;
}

inline bool SettingsSceneModel::setWindowRow(uint8_t index, const char* id, const char* title, const char* value, bool isSection, bool selected) {
  if (index >= kMaxWindowRows) return false;
  if (!id) id = "";
  if (!title) title = "";
  if (!value) value = "";
  const char* vals[] = {id, title, value};
  if (!canAppend(vals, 3)) return false;
  auto& row = draft_.window[index];
  row.id = appendText(id);
  row.title = appendText(title);
  row.value = appendText(value);
  row.isSection = isSection;
  row.isRow = !isSection && (title[0] != '\0' || id[0] != '\0' || value[0] != '\0' || selected);
  // For empty補空, caller may set isSection=false and title empty and selected false -> isRow false
  if (isSection) row.isRow = false;
  row.selected = selected;
  if (draft_.windowCount < kMaxWindowRows) draft_.windowCount = static_cast<uint8_t>(kMaxWindowRows);
  return true;
}

inline bool SettingsSceneModel::clearWindow() {
  for (std::size_t i = 0; i < kMaxWindowRows; ++i) {
    draft_.window[i] = SettingsWindowRow{};
  }
  draft_.windowCount = static_cast<uint8_t>(kMaxWindowRows);
  return true;
}

inline bool SettingsSceneModel::populateHubFromPolicy() {
  for (int i = 0; i < kSettingsHubCardCount; ++i) {
    auto card = static_cast<SettingsHubCard>(i);
    const char* title = settingsHubCardTitleZh(card);
    char idBuf[8]{};
    // simple id as string of index
    idBuf[0] = static_cast<char>('0' + i);
    idBuf[1] = '\0';
    // we keep id as index string; ignore overflow
    if (!setHubCard(static_cast<uint8_t>(i), idBuf, title)) return false;
  }
  draft_.hubCount = kSettingsHubCardCount;
  return true;
}

inline bool SettingsSceneModel::populateWindowFromPolicy(SettingsHubCard card, const SettingsNavState& nav, bool m4Build) {
  clearWindow();
  const int flatCount = settingsFlatCount(card, m4Build);
  for (int i = 0; i < static_cast<int>(kMaxWindowRows); ++i) {
    int flatIndex = nav.windowStart + i;
    SettingsWindowRow out{};
    if (flatIndex < flatCount) {
      auto fr = settingsFlatAt(card, flatIndex, m4Build);
      const char* key = fr.key ? fr.key : "";
      const char* title = fr.titleZh ? fr.titleZh : "";
      bool isSection = (fr.kind == SettingsFlatKind::Section);
      // selected only for setting rows that correspond to nav.selectedRow
      bool selected = false;
      if (!isSection) {
        int flatOfSelected = settingsFlatIndexOfSetting(card, nav.selectedRow, m4Build);
        selected = (flatIndex == flatOfSelected);
      }
      // For setting rows, value is placeholder empty (real value resolved elsewhere)
      // We store title as key/titleZh; value empty for now.
      if (!setWindowRow(static_cast<uint8_t>(i), isSection ? "" : key, title, "", isSection, selected)) return false;
    } else {
      // empty补空
      if (!setWindowRow(static_cast<uint8_t>(i), "", "", "", false, false)) return false;
      // force isRow false for empty
      draft_.window[i].isRow = false;
      draft_.window[i].isSection = false;
      draft_.window[i].selected = false;
    }
  }
  return true;
}

inline bool SettingsSceneModel::copyLatest(SettingsSnapshot& out) const {
  if (!published_.load(std::memory_order_acquire)) return false;
  auto snap = store_.acquire();
  if (!snap.valid()) return false;
  out = snap.value();
  return true;
}

inline bool SettingsSceneModel::hasPublishedSnapshot() const { return published_.load(std::memory_order_acquire); }
inline uint32_t SettingsSceneModel::latestRevision() const { SettingsSnapshot s{}; return copyLatest(s) ? s.revision : 0; }

inline UiSceneRuntime::SceneBindingSource SettingsSceneModel::bindingSource(const SettingsSnapshot& snapshot) {
  return {&snapshot, &SettingsSceneModel::resolve, &SettingsSceneModel::count};
}

inline bool SettingsSceneModel::resolve(const void* user, UiScene::BindingId binding, const UiSceneRuntime::SceneItemContext* item, UiSceneRuntime::ResolvedValue* out) {
  if (!user || !out) return false;
  const auto& snap = *static_cast<const SettingsSnapshot*>(user);
  *out = UiSceneRuntime::ResolvedValue{};
  if (binding == kBindingSystemBattery) {
    out->kind = UiSceneRuntime::ValueKind::Int;
    out->number = snap.battery;
    return true;
  }
  if (binding == kBindingPageTitle) {
    out->kind = UiSceneRuntime::ValueKind::Text;
    out->text = snap.textView(snap.pageTitle);
    return out->text.data != nullptr || snap.pageTitle.length == 0;
  }
  if (!item || !item->valid) {
    // non-item bindings already handled
    return false;
  }
  if (item->sourceBinding == kBindingHubCards && item->index < snap.hubCount) {
    SettingsTextRef ref{};
    if (binding == kBindingItemTitle) ref = snap.hubTitles[item->index];
    else if (binding == kBindingItemId) ref = snap.hubIds[item->index];
    else if (binding == kBindingItemSelected) {
      out->kind = UiSceneRuntime::ValueKind::Bool;
      out->boolean = (snap.pane == SettingsPane::Hub && static_cast<int>(snap.hub) == item->index);
      return true;
    } else return false;
    out->kind = UiSceneRuntime::ValueKind::Text;
    out->text = snap.textView(ref);
    return out->text.data != nullptr || ref.length == 0;
  }
  if (item->sourceBinding == kBindingPageRows && item->index < snap.windowCount) {
    const auto& row = snap.window[item->index];
    if (binding == kBindingItemTitle) {
      out->kind = UiSceneRuntime::ValueKind::Text;
      out->text = snap.textView(row.title);
      return out->text.data != nullptr || row.title.length == 0;
    } else if (binding == kBindingItemValue) {
      out->kind = UiSceneRuntime::ValueKind::Text;
      out->text = snap.textView(row.value);
      return out->text.data != nullptr || row.value.length == 0;
    } else if (binding == kBindingItemId) {
      out->kind = UiSceneRuntime::ValueKind::Text;
      out->text = snap.textView(row.id);
      return out->text.data != nullptr || row.id.length == 0;
    } else if (binding == kBindingItemSelected) {
      out->kind = UiSceneRuntime::ValueKind::Bool;
      out->boolean = row.selected;
      return true;
    } else if (binding == kBindingItemIsSection) {
      out->kind = UiSceneRuntime::ValueKind::Bool;
      out->boolean = row.isSection;
      return true;
    } else if (binding == kBindingItemIsRow) {
      out->kind = UiSceneRuntime::ValueKind::Bool;
      out->boolean = row.isRow;
      return true;
    }
  }
  return false;
}

inline uint8_t SettingsSceneModel::count(const void* user, UiScene::BindingId source) {
  if (!user) return 0;
  const auto& snap = *static_cast<const SettingsSnapshot*>(user);
  if (source == kBindingHubCards) return snap.pane == SettingsPane::Hub ? snap.hubCount : 0;
  if (source == kBindingPageRows) return snap.pane == SettingsPane::Category ? snap.windowCount : 0;
  // also allow unconditional for theme compiler preview where pane may be default
  if (source == kBindingHubCards) return snap.hubCount;
  if (source == kBindingPageRows) return snap.windowCount;
  return 0;
}

inline bool SettingsSceneModel::actionTarget(const SettingsSnapshot& snapshot, UiScene::ActionId action, const UiSceneRuntime::SceneItemContext* item, SettingsActionTarget* out) {
  if (!out) return false;
  *out = SettingsActionTarget{};
  out->action = action;
  if (action == kActionOpenHubCard && item && item->valid && item->sourceBinding == kBindingHubCards && item->index < snapshot.hubCount) {
    out->itemIndex = item->index;
    auto tv = snapshot.textView(snapshot.hubIds[item->index]);
    out->argumentLength = tv.size < kMaxActionArgumentBytes ? static_cast<uint8_t>(tv.size) : static_cast<uint8_t>(kMaxActionArgumentBytes);
    for (uint8_t i = 0; i < out->argumentLength; ++i) out->argument[i] = static_cast<char>(tv.readByte(i));
    return true;
  }
  if (action == kActionActivateSetting && item && item->valid && item->sourceBinding == kBindingPageRows && item->index < snapshot.windowCount) {
    const auto& row = snapshot.window[item->index];
    if (row.isSection) return false; // section not activatable
    out->itemIndex = item->index;
    auto tv = snapshot.textView(row.id);
    out->argumentLength = tv.size < kMaxActionArgumentBytes ? static_cast<uint8_t>(tv.size) : static_cast<uint8_t>(kMaxActionArgumentBytes);
    for (uint8_t i = 0; i < out->argumentLength; ++i) out->argument[i] = static_cast<char>(tv.readByte(i));
    return true;
  }
  return false;
}

inline bool SettingsSceneModel::actionTarget(UiScene::ActionId action, const UiSceneRuntime::SceneItemContext* item, SettingsActionTarget* out) const {
  SettingsSnapshot snap{};
  return copyLatest(snap) && actionTarget(snap, action, item, out);
}

} // namespace SettingsScene
