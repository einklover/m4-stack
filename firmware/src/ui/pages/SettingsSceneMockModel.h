#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "ui/scene/UiSceneRuntime.h"
#include "ui/scene/UiStateStore.h"
#include "ui/scene/UiSceneTypes.h"

namespace SettingsSceneMock {

// Numeric ABI — must match themes/settings-scene-mock/theme.json bindings/actions
// and stay stable for the generated package. Keep these IDs bounded 1..254
// and non-colliding with reserved common/legacy IDs (1,2,10-15,20,21,30-35).
constexpr UiScene::BindingId kBindingSystemBattery = 1;
constexpr UiScene::BindingId kBindingPageSettings = 64;
constexpr UiScene::BindingId kBindingPageStatus = 65;
constexpr UiScene::BindingId kBindingItemValue = 66;
constexpr UiScene::BindingId kBindingItemEnabled = 67;
constexpr UiScene::BindingId kBindingItemTitle = 32;
constexpr UiScene::BindingId kBindingItemId = 30;

constexpr UiScene::ActionId kActionOpenSetting = 32;
constexpr UiScene::ActionId kActionToggleSetting = 33;

constexpr uint8_t kInvalidItemIndex = 0xFF;
constexpr std::size_t kMaxSettingsItems = 5;
constexpr std::size_t kMaxTextBytes = 1536;
constexpr std::size_t kMaxActionArgumentBytes = 64;

struct SettingsTextRef {
  uint16_t offset = 0;
  uint16_t length = 0;
};

struct SettingsItem {
  SettingsTextRef id{};
  SettingsTextRef title{};
  SettingsTextRef value{};
  bool enabled = false;
};

// Plain value snapshot — no provider, filesystem, or heap pointers cross UI.
struct SettingsSnapshot {
  uint32_t revision = 0;
  UiScene::DataState state = UiScene::DataState::Loading;
  uint8_t errorCode = 0;
  bool stale = false;
  int32_t battery = 0;
  SettingsTextRef status{};
  uint8_t itemCount = 0;
  SettingsItem items[kMaxSettingsItems]{};
  uint16_t textUsed = 0;
  char text[kMaxTextBytes]{};

  UiSceneRuntime::TextView textView(SettingsTextRef ref) const {
    if (ref.length == 0 || ref.offset > textUsed ||
        ref.length > textUsed - ref.offset) return {};
    return UiSceneRuntime::TextView::fromRam(text + ref.offset, ref.length);
  }
};

struct SettingsActionTarget {
  UiScene::ActionId action = UiScene::kInvalidActionId;
  uint8_t itemIndex = kInvalidItemIndex;
  uint16_t itemKey = 0;
  uint8_t argumentLength = 0;
  char argument[kMaxActionArgumentBytes]{};

  UiSceneRuntime::TextView argumentView() const {
    return argumentLength == 0
               ? UiSceneRuntime::TextView{}
               : UiSceneRuntime::TextView::fromRam(argument, argumentLength);
  }
};

class SettingsSceneMockModel final {
 public:
  SettingsSceneMockModel();

  void begin(UiScene::DataState state);
  bool publish();
  bool publishLoading();
  bool publishEmpty(uint8_t errorCode = 0);
  bool publishError(uint8_t errorCode);
  bool publishStale();

  bool setBattery(int32_t percent);
  bool setStatus(const char* statusText);
  bool addItem(const char* id, const char* title, const char* value,
               bool enabled);

  bool copyLatest(SettingsSnapshot& out) const;
  bool hasPublishedSnapshot() const;
  uint32_t latestRevision() const;

  static UiSceneRuntime::SceneBindingSource bindingSource(
      const SettingsSnapshot& snapshot);
  static bool actionTarget(const SettingsSnapshot& snapshot,
                           UiScene::ActionId action,
                           const UiSceneRuntime::SceneItemContext* item,
                           SettingsActionTarget* out);

  bool actionTarget(UiScene::ActionId action,
                    const UiSceneRuntime::SceneItemContext* item,
                    SettingsActionTarget* out) const;
  UiSceneRuntime::TextView text(SettingsTextRef ref) const {
    return draft_.textView(ref);
  }

 private:
  static constexpr std::size_t kMaxStringProbe = kMaxTextBytes + 1;

  SettingsSnapshot draft_{};
  mutable UiStateStore<SettingsSnapshot> store_;
  std::atomic<bool> published_{false};
  uint32_t nextRevision_ = 1;

  static SettingsSnapshot initialSnapshot();
  static std::size_t boundedLength(const char* value);
  bool canAppend(const char* const* values, std::size_t count) const;
  SettingsTextRef appendText(const char* value);
  static bool resolve(const void* user, UiScene::BindingId binding,
                      const UiSceneRuntime::SceneItemContext* item,
                      UiSceneRuntime::ResolvedValue* out);
  static uint8_t count(const void* user, UiScene::BindingId source);
};

inline SettingsSnapshot SettingsSceneMockModel::initialSnapshot() {
  SettingsSnapshot snapshot{};
  snapshot.state = UiScene::DataState::Loading;
  return snapshot;
}

inline SettingsSceneMockModel::SettingsSceneMockModel()
    : store_(initialSnapshot()) {
  draft_ = initialSnapshot();
}

inline std::size_t SettingsSceneMockModel::boundedLength(const char* value) {
  if (!value) return 0;
  std::size_t length = 0;
  while (length < kMaxStringProbe && value[length] != '\0') ++length;
  return length;
}

inline bool SettingsSceneMockModel::canAppend(const char* const* values,
                                              std::size_t count) const {
  std::size_t needed = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t length = boundedLength(values[i]);
    if (length >= kMaxStringProbe) return false;
    needed += length;
  }
  return needed <= kMaxTextBytes - draft_.textUsed;
}

inline SettingsTextRef SettingsSceneMockModel::appendText(const char* value) {
  const std::size_t length = boundedLength(value);
  if (length >= kMaxStringProbe ||
      length > kMaxTextBytes - draft_.textUsed) return {};
  const SettingsTextRef ref{draft_.textUsed,
                            static_cast<uint16_t>(length)};
  if (length != 0) std::memcpy(draft_.text + draft_.textUsed, value, length);
  draft_.textUsed = static_cast<uint16_t>(draft_.textUsed + length);
  return ref;
}

inline void SettingsSceneMockModel::begin(UiScene::DataState state) {
  draft_ = SettingsSnapshot{};
  draft_.state = state;
}

inline bool SettingsSceneMockModel::publish() {
  draft_.revision = nextRevision_++;
  if (!store_.tryPublish(draft_)) return false;
  published_.store(true, std::memory_order_release);
  return true;
}

inline bool SettingsSceneMockModel::publishLoading() {
  begin(UiScene::DataState::Loading);
  return publish();
}

inline bool SettingsSceneMockModel::publishEmpty(uint8_t errorCode) {
  begin(UiScene::DataState::Empty);
  draft_.errorCode = errorCode;
  return publish();
}

inline bool SettingsSceneMockModel::publishError(uint8_t errorCode) {
  begin(UiScene::DataState::Error);
  draft_.errorCode = errorCode;
  return publish();
}

inline bool SettingsSceneMockModel::publishStale() {
  SettingsSnapshot snapshot{};
  if (!copyLatest(snapshot)) return false;
  snapshot.state = UiScene::DataState::Stale;
  snapshot.stale = true;
  snapshot.revision = nextRevision_++;
  if (!store_.tryPublish(snapshot)) return false;
  published_.store(true, std::memory_order_release);
  return true;
}

inline bool SettingsSceneMockModel::setBattery(int32_t percent) {
  draft_.battery = percent;
  return true;
}

inline bool SettingsSceneMockModel::setStatus(const char* statusText) {
  if (!canAppend(&statusText, 1)) return false;
  draft_.status = appendText(statusText);
  return true;
}

inline bool SettingsSceneMockModel::addItem(const char* id, const char* title,
                                            const char* value, bool enabled) {
  if (draft_.itemCount >= kMaxSettingsItems) return false;
  const char* values[] = {id, title, value};
  if (!canAppend(values, 3)) return false;
  SettingsItem& item = draft_.items[draft_.itemCount++];
  item.id = appendText(id);
  item.title = appendText(title);
  item.value = appendText(value);
  item.enabled = enabled;
  return true;
}

inline bool SettingsSceneMockModel::copyLatest(SettingsSnapshot& out) const {
  if (!published_.load(std::memory_order_acquire)) return false;
  auto snapshot = store_.acquire();
  if (!snapshot.valid()) return false;
  out = snapshot.value();
  return true;
}

inline bool SettingsSceneMockModel::hasPublishedSnapshot() const {
  return published_.load(std::memory_order_acquire);
}

inline uint32_t SettingsSceneMockModel::latestRevision() const {
  SettingsSnapshot snapshot{};
  return copyLatest(snapshot) ? snapshot.revision : 0;
}

inline UiSceneRuntime::SceneBindingSource
SettingsSceneMockModel::bindingSource(const SettingsSnapshot& snapshot) {
  return {&snapshot, &SettingsSceneMockModel::resolve,
          &SettingsSceneMockModel::count};
}

inline bool SettingsSceneMockModel::resolve(
    const void* user, UiScene::BindingId binding,
    const UiSceneRuntime::SceneItemContext* item,
    UiSceneRuntime::ResolvedValue* out) {
  if (!user || !out) return false;
  const auto& snapshot = *static_cast<const SettingsSnapshot*>(user);
  *out = UiSceneRuntime::ResolvedValue{};
  if (binding == kBindingSystemBattery) {
    out->kind = UiSceneRuntime::ValueKind::Int;
    out->number = snapshot.battery;
    return true;
  }
  if (binding == kBindingPageStatus) {
    // Settings status is Text for the main status node. The three
    // supplementary "Loading..."/"Ready"/"Error" literals use the same
    // binding as visible_if (which expects Bool). Returning Text here
    // intentionally makes those supplementary nodes hidden — the generic
    // runtime's visible() checks Bool, so they are not emitted. This
    // preserves the same runtime without special casing and still proves
    // header/status/repeated rows via the main status Text node.
    out->kind = UiSceneRuntime::ValueKind::Text;
    out->text = snapshot.textView(snapshot.status);
    return out->text.data != nullptr || snapshot.status.length == 0;
  }
  if (!item || !item->valid) return false;
  if (item->sourceBinding == kBindingPageSettings &&
      item->index < snapshot.itemCount) {
    const auto& entry = snapshot.items[item->index];
    SettingsTextRef ref{};
    if (binding == kBindingItemTitle) ref = entry.title;
    else if (binding == kBindingItemValue) ref = entry.value;
    else if (binding == kBindingItemId) ref = entry.id;
    else if (binding == kBindingItemEnabled) {
      // Enabled is used as icon binding. Returning Text ensures the
      // generic icon node emits (runtime expects Text or Asset, not Bool).
      // Use "on"/"off" literal so Gfx can differentiate if needed.
      out->kind = UiSceneRuntime::ValueKind::Text;
      out->text = entry.enabled
                      ? UiSceneRuntime::TextView::fromRam("on", 2)
                      : UiSceneRuntime::TextView::fromRam("off", 3);
      return true;
    } else {
      return false;
    }
    out->kind = UiSceneRuntime::ValueKind::Text;
    out->text = snapshot.textView(ref);
    return out->text.data != nullptr || ref.length == 0;
  }
  return false;
}

inline uint8_t SettingsSceneMockModel::count(const void* user,
                                             UiScene::BindingId source) {
  if (!user) return 0;
  const auto& snapshot = *static_cast<const SettingsSnapshot*>(user);
  if (source == kBindingPageSettings) return snapshot.itemCount;
  return 0;
}

inline bool SettingsSceneMockModel::actionTarget(
    const SettingsSnapshot& snapshot, UiScene::ActionId action,
    const UiSceneRuntime::SceneItemContext* item,
    SettingsActionTarget* out) {
  if (!out) return false;
  *out = SettingsActionTarget{};
  out->action = action;
  if (action == kActionOpenSetting) {
    // Footer Back — no item required, always navigates
    if (item && item->valid) {
      // If invoked from repeat (toggle), item will be valid; still allow but
      // treat as per-item open
      out->itemIndex = item->index;
    }
    return true;
  }
  if (action == kActionToggleSetting && item && item->valid &&
      item->sourceBinding == kBindingPageSettings &&
      item->index < snapshot.itemCount) {
    out->itemIndex = item->index;
    const auto text = snapshot.textView(snapshot.items[item->index].id);
    out->argumentLength =
        text.size < kMaxActionArgumentBytes
            ? static_cast<uint8_t>(text.size)
            : static_cast<uint8_t>(kMaxActionArgumentBytes);
    for (uint8_t i = 0; i < out->argumentLength; ++i) {
      out->argument[i] = static_cast<char>(text.readByte(i));
    }
    return true;
  }
  return false;
}

inline bool SettingsSceneMockModel::actionTarget(
    UiScene::ActionId action, const UiSceneRuntime::SceneItemContext* item,
    SettingsActionTarget* out) const {
  SettingsSnapshot snapshot{};
  return copyLatest(snapshot) && actionTarget(snapshot, action, item, out);
}

}  // namespace SettingsSceneMock
