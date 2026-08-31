#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "ui/scene/UiSceneRuntime.h"
#include "ui/scene/UiSceneActionQueue.h"
#include "ui/scene/UiSceneAssets.h"
#include "ui/scene/UiStateStore.h"

namespace HomeScene {

// IDs are part of the generated Home scene ABI. Keep them numeric and stable.
constexpr UiScene::BindingId kBindingSystemBattery = 1;
constexpr UiScene::BindingId kBindingWifiConnected = 2;
constexpr UiScene::BindingId kBindingCurrentExists = 10;
constexpr UiScene::BindingId kBindingCurrentTitle = 11;
constexpr UiScene::BindingId kBindingCurrentAuthor = 12;
constexpr UiScene::BindingId kBindingCurrentSource = 13;
constexpr UiScene::BindingId kBindingCurrentCover = 14;
constexpr UiScene::BindingId kBindingCurrentProgress = 15;
constexpr UiScene::BindingId kBindingCurrentProgressText = 16;
constexpr UiScene::BindingId kBindingRecent = 20;
constexpr UiScene::BindingId kBindingApps = 21;
constexpr UiScene::BindingId kBindingItemId = 30;
constexpr UiScene::BindingId kBindingItemName = 31;
constexpr UiScene::BindingId kBindingItemTitle = 32;
constexpr UiScene::BindingId kBindingItemCover = 33;
constexpr UiScene::BindingId kBindingItemIcon = 34;
constexpr UiScene::BindingId kBindingItemProgress = 35;

constexpr UiScene::ActionId kActionOpenCurrentBook = 0;
constexpr UiScene::ActionId kActionOpenHistory = 1;
constexpr UiScene::ActionId kActionOpenApps = 2;
constexpr UiScene::ActionId kActionOpenApp = 3;

constexpr uint8_t kInvalidItemIndex = 0xFF;
constexpr std::size_t kMaxRecentItems = 4;
constexpr std::size_t kMaxAppItems = 4;
constexpr std::size_t kMaxTextBytes = 1536;
constexpr std::size_t kMaxActionArgumentBytes = UiScene::kUiSceneActionArgumentBytes;

struct HomeTextRef {
  uint16_t offset = 0;
  uint16_t length = 0;
};

struct HomeRecentItem {
  HomeTextRef path{};
  HomeTextRef originalSource{};
  HomeTextRef title{};
  HomeTextRef author{};
  HomeTextRef source{};
  HomeTextRef cover{};
  int32_t progress = 0;
};

struct HomeAppItem {
  HomeTextRef id{};
  HomeTextRef name{};
  HomeTextRef icon{};
};

// This is intentionally a plain value: no provider, filesystem, callback,
// STL string/container, or pointer is allowed to cross the UI boundary.
struct HomeSceneSnapshot {
  uint32_t revision = 0;
  UiScene::DataState state = UiScene::DataState::Loading;
  uint8_t errorCode = 0;
  bool stale = false;
  bool wifiConnected = false;
  bool currentExists = false;
  uint8_t selectedIndex = 0;
  uint8_t recentCount = 0;
  uint8_t appCount = 0;
  int32_t battery = 0;
  int32_t currentProgress = 0;
  HomeTextRef currentProgressText{};
  HomeTextRef currentTitle{};
  HomeTextRef currentAuthor{};
  HomeTextRef currentSource{};
  HomeTextRef currentCover{};
  HomeTextRef currentPath{};
  HomeTextRef currentOriginalSource{};
  HomeRecentItem recent[kMaxRecentItems]{};
  HomeAppItem apps[kMaxAppItems]{};
  uint16_t textUsed = 0;
  char text[kMaxTextBytes]{};

  UiSceneRuntime::TextView textView(HomeTextRef ref) const {
    if (ref.length == 0 || ref.offset > textUsed ||
        ref.length > textUsed - ref.offset) return {};
    return UiSceneRuntime::TextView::fromRam(text + ref.offset, ref.length);
  }
};

// Paired asset geometry — keep in sync with M4QemuHomeSceneFixture and audit.
constexpr uint16_t kHomeCurrentCoverW = 110;
constexpr uint16_t kHomeCurrentCoverH = 180;
constexpr uint16_t kHomeCurrentCoverStride = 14;
constexpr size_t kHomeCurrentCoverBytes = 2520;

constexpr uint16_t kHomeRecentCoverW = 74;
constexpr uint16_t kHomeRecentCoverH = 106;
constexpr uint16_t kHomeRecentCoverStride = 10;
constexpr size_t kHomeRecentCoverBytes = 1060;

constexpr uint16_t kHomeAppIconW = 62;
constexpr uint16_t kHomeAppIconH = 64;
constexpr uint16_t kHomeAppIconStride = 8;
constexpr size_t kHomeAppIconBytes = 512;

constexpr size_t kHomeAssetArenaBytes = 7748; // 2520 + 3*1060 + 4*512
constexpr size_t kMaxHomeAssets = 8;

struct HomePublicationAssetEntry {
  UiScene::AssetKey key{};
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t stride = 0;
  uint16_t offset = 0;
  bool valid = false;
};

struct HomeScenePublication {
  HomeSceneSnapshot snapshot{};
  uint8_t assetCount = 0;
  HomePublicationAssetEntry entries[kMaxHomeAssets]{};
  uint8_t arena[kHomeAssetArenaBytes]{};
};

static_assert(std::is_trivially_copyable<HomeScenePublication>::value,
              "HomeScenePublication must be trivially copyable for UiStateStore");

inline bool homePublicationSlotForKey(const UiScene::AssetKey& key, size_t* outOffset,
                                      uint16_t* outW, uint16_t* outH, uint16_t* outStride,
                                      size_t* outBytes) {
  if (!outOffset || !outW || !outH || !outStride || !outBytes) return false;
  using UiScene::kInvalidBindingId;
  using UiScene::kInvalidAssetItemIndex;
  if (key.binding == kBindingCurrentCover && key.sourceBinding == kInvalidBindingId &&
      key.itemIndex == kInvalidAssetItemIndex) {
    *outOffset = 0;
    *outW = kHomeCurrentCoverW;
    *outH = kHomeCurrentCoverH;
    *outStride = kHomeCurrentCoverStride;
    *outBytes = kHomeCurrentCoverBytes;
    return true;
  }
  if (key.binding == kBindingItemCover && key.sourceBinding == kBindingRecent) {
    if (key.itemIndex >= 3) return false;
    *outOffset = kHomeCurrentCoverBytes + static_cast<size_t>(key.itemIndex) * kHomeRecentCoverBytes;
    *outW = kHomeRecentCoverW;
    *outH = kHomeRecentCoverH;
    *outStride = kHomeRecentCoverStride;
    *outBytes = kHomeRecentCoverBytes;
    return true;
  }
  if (key.binding == kBindingItemIcon && key.sourceBinding == kBindingApps) {
    if (key.itemIndex >= 4) return false;
    *outOffset = kHomeCurrentCoverBytes + 3 * kHomeRecentCoverBytes +
                 static_cast<size_t>(key.itemIndex) * kHomeAppIconBytes;
    *outW = kHomeAppIconW;
    *outH = kHomeAppIconH;
    *outStride = kHomeAppIconStride;
    *outBytes = kHomeAppIconBytes;
    return true;
  }
  return false;
}

inline bool homeAddAssetToPublication(HomeScenePublication& pub, const UiScene::AssetKey& key,
                                      const uint8_t* data, uint16_t w, uint16_t h,
                                      uint16_t stride) {
  if (!data || pub.assetCount >= kMaxHomeAssets) return false;
  size_t offset = 0;
  uint16_t expW = 0, expH = 0, expStride = 0;
  size_t expBytes = 0;
  if (!homePublicationSlotForKey(key, &offset, &expW, &expH, &expStride, &expBytes)) return false;
  if (w != expW || h != expH || stride != expStride) return false;
  if (offset + expBytes > kHomeAssetArenaBytes) return false;
  for (uint8_t i = 0; i < pub.assetCount; ++i) {
    if (pub.entries[i].key == key) return false;
  }
  // Decoders often write into the slot first; overlapping memcpy is UB.
  if (data != pub.arena + offset) {
    std::memcpy(pub.arena + offset, data, expBytes);
  }
  HomePublicationAssetEntry e{};
  e.key = key;
  e.width = w;
  e.height = h;
  e.stride = stride;
  e.offset = static_cast<uint16_t>(offset);
  e.valid = true;
  pub.entries[pub.assetCount++] = e;
  return true;
}

inline void homePublicationToAssets(const HomeScenePublication& pub, UiScene::UiSceneAssets& out) {
  out.clear();
  for (uint8_t i = 0; i < pub.assetCount; ++i) {
    const auto& e = pub.entries[i];
    if (!e.valid) continue;
    if (static_cast<size_t>(e.offset) + static_cast<size_t>(e.stride) * e.height > kHomeAssetArenaBytes) continue;
    UiScene::UiSceneAsset asset{};
    asset.pixels = pub.arena + e.offset;
    asset.width = e.width;
    asset.height = e.height;
    asset.stride = e.stride;
    asset.progmem = false;
    out.add(e.key, asset);
  }
}

using HomeSceneActionTarget = UiScene::UiSceneAction;

class HomeSceneModel final {
 public:
  HomeSceneModel();
  ~HomeSceneModel();

  void begin(UiScene::DataState state);
  bool publish();
  bool publishLoading();
  bool publishEmpty(uint8_t errorCode = 0);
  bool publishError(uint8_t errorCode);
  bool publishStale();

  bool setBattery(int32_t percent);
  bool setWifiConnected(bool connected);
  bool setSelectedIndex(uint8_t index);
  bool setCurrent(const char* title, const char* author, const char* source,
                  const char* cover, int32_t progress);
  bool setCurrentPaths(const char* path, const char* originalSource);
  bool addRecent(const char* title, const char* author, const char* source,
                 const char* cover, int32_t progress);
  bool setRecentPaths(uint8_t index, const char* path, const char* originalSource);
  bool addApp(const char* id, const char* name, const char* icon);

  bool copyLatest(HomeSceneSnapshot& out) const;
  bool hasPublishedSnapshot() const;
  uint32_t latestRevision() const;

  static UiSceneRuntime::SceneBindingSource bindingSource(
      const HomeSceneSnapshot& snapshot);
  static bool actionTarget(const HomeSceneSnapshot& snapshot,
                           UiScene::ActionId action,
                           const UiSceneRuntime::SceneItemContext* item,
                           HomeSceneActionTarget* out);

  // Convenience for callers that already use the model as their page adapter.
  bool actionTarget(UiScene::ActionId action,
                    const UiSceneRuntime::SceneItemContext* item,
                    HomeSceneActionTarget* out) const;

  // Paired publication (snapshot + fixed asset arena) — renderer pins this for the full frame.
  bool publishWithAssets(const HomeScenePublication& pub);
  bool copyLatestPublication(HomeScenePublication& out) const;
  UiStateStore<HomeScenePublication>::Snapshot acquirePublication();
  bool addPublicationAsset(const UiScene::AssetKey& key, const uint8_t* data,
                           uint16_t w, uint16_t h, uint16_t stride);
  HomeScenePublication& draftPublication() { return *draftPubPtr_; }
  const HomeScenePublication& draftPublication() const { return *draftPubPtr_; }
  // addApp/addRecent mutate draft_, not draftPublication().snapshot. Icon decode
  // must read this snapshot; publication.snapshot stays empty until publish().
  const HomeSceneSnapshot& draftSnapshot() const { return draft_; }
  // For storage contract tests: whether draft arena is heap/PSRAM.
  bool isDraftHeapAllocated() const { return draftHeapAllocated_; }

 private:
  static constexpr std::size_t kMaxStringProbe = kMaxTextBytes + 1;

  HomeSceneSnapshot draft_{};
  HomeScenePublication* draftPubPtr_ = nullptr;
  bool draftHeapAllocated_ = false;
  // Legacy snapshot store kept for compatibility; new paired store is authoritative.
  mutable UiStateStore<HomeSceneSnapshot> store_;
  mutable UiStateStore<HomeScenePublication> pubStore_;
  std::atomic<bool> published_{false};
  std::atomic<bool> pubPublished_{false};
  uint32_t nextRevision_ = 1;

  static HomeSceneSnapshot initialSnapshot();
  static std::size_t boundedLength(const char* value);
  bool canAppend(const char* const* values, std::size_t count) const;
  HomeTextRef appendText(const char* value);
  static bool resolve(const void* user, UiScene::BindingId binding,
                      const UiSceneRuntime::SceneItemContext* item,
                      UiSceneRuntime::ResolvedValue* out);
  static uint8_t count(const void* user, UiScene::BindingId source);
};

}  // namespace HomeScene
