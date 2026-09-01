#include "ui/pages/HomeSceneModel.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef CROSSPOINT_MURPHY_M4
#include <esp_heap_caps.h>
#endif

namespace HomeScene {

HomeSceneSnapshot HomeSceneModel::initialSnapshot() {
  HomeSceneSnapshot snapshot{};
  snapshot.state = UiScene::DataState::Loading;
  const char* defaultBrand = "Murphy M4";
  size_t len = 0;
  while (defaultBrand[len] != '\0' && len < kMaxTextBytes) ++len;
  if (len > 0 && len < kMaxTextBytes) {
    std::memcpy(snapshot.text, defaultBrand, len);
    snapshot.brandText = HomeTextRef{0, static_cast<uint16_t>(len)};
    snapshot.textUsed = static_cast<uint16_t>(len);
  }
  return snapshot;
}

HomeSceneModel::HomeSceneModel()
    : store_(initialSnapshot()),
      pubStore_(HomeScenePublication{initialSnapshot(), 0, {}, {}}) {
  draft_ = initialSnapshot();
  // Allocate draft publication arena in PSRAM/heap to keep internal RAM free.
  // Large arena (8148 B) would otherwise consume ~1/3 of the 32.6KB internal budget.
  HomeScenePublication* heap = nullptr;
#ifdef CROSSPOINT_MURPHY_M4
  heap = static_cast<HomeScenePublication*>(
      heap_caps_malloc(sizeof(HomeScenePublication), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!heap) heap = static_cast<HomeScenePublication*>(
      heap_caps_malloc(sizeof(HomeScenePublication), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
  if (!heap) heap = static_cast<HomeScenePublication*>(malloc(sizeof(HomeScenePublication)));
#else
  heap = static_cast<HomeScenePublication*>(malloc(sizeof(HomeScenePublication)));
#endif
  if (heap) {
    draftPubPtr_ = heap;
    draftHeapAllocated_ = true;
    *draftPubPtr_ = HomeScenePublication{};
  } else {
    // Fallback: try malloc again (should not happen in practice)
    heap = static_cast<HomeScenePublication*>(malloc(sizeof(HomeScenePublication)));
    if (heap) {
      draftPubPtr_ = heap;
      draftHeapAllocated_ = true;
      *draftPubPtr_ = HomeScenePublication{};
    } else {
      // Last resort: allocate static fallback (leaks but keeps functional)
      static HomeScenePublication fallback{};
      draftPubPtr_ = &fallback;
      draftHeapAllocated_ = false;
    }
  }
  draftPubPtr_->snapshot = draft_;
  draftPubPtr_->assetCount = 0;
  std::memset(draftPubPtr_->arena, 0, kHomeAssetArenaBytes);
}

HomeSceneModel::~HomeSceneModel() {
  if (draftHeapAllocated_ && draftPubPtr_) {
#ifdef CROSSPOINT_MURPHY_M4
    heap_caps_free(draftPubPtr_);
#else
    free(draftPubPtr_);
#endif
    draftPubPtr_ = nullptr;
  }
}

std::size_t HomeSceneModel::boundedLength(const char* value) {
  if (!value) return 0;
  std::size_t length = 0;
  while (length < kMaxStringProbe && value[length] != '\0') ++length;
  return length;
}

bool HomeSceneModel::canAppend(const char* const* values, std::size_t count) const {
  std::size_t needed = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t length = boundedLength(values[i]);
    if (length >= kMaxStringProbe) return false;
    needed += length;
  }
  return needed <= kMaxTextBytes - draft_.textUsed;
}

HomeTextRef HomeSceneModel::appendText(const char* value) {
  const std::size_t length = boundedLength(value);
  if (length >= kMaxStringProbe || length > kMaxTextBytes - draft_.textUsed) return {};
  const HomeTextRef ref{draft_.textUsed, static_cast<uint16_t>(length)};
  if (length != 0) std::memcpy(draft_.text + draft_.textUsed, value, length);
  draft_.textUsed = static_cast<uint16_t>(draft_.textUsed + length);
  return ref;
}

void HomeSceneModel::begin(UiScene::DataState state) {
  draft_ = HomeSceneSnapshot{};
  draft_.state = state;
  const char* defaultBrand = "Murphy M4";
  size_t len = 0;
  while (defaultBrand[len] != '\0' && len < kMaxTextBytes) ++len;
  if (len > 0 && len < kMaxTextBytes) {
    std::memcpy(draft_.text, defaultBrand, len);
    draft_.brandText = HomeTextRef{0, static_cast<uint16_t>(len)};
    draft_.textUsed = static_cast<uint16_t>(len);
  }
  *draftPubPtr_ = HomeScenePublication{};
  draftPubPtr_->snapshot = draft_;
}

bool HomeSceneModel::publish() {
  draft_.revision = nextRevision_++;
  draftPubPtr_->snapshot = draft_;
  draftPubPtr_->snapshot.revision = draft_.revision;
  // Paired publication is authoritative: snapshot+assets must atomically succeed.
  // Legacy snapshot store is only mirrored after paired success for compat, and never
  // determines success (prevents updateRequired for failed asset publish).
  bool pubOk = pubStore_.tryPublish(*draftPubPtr_);
  if (!pubOk) return false;
  pubPublished_.store(true, std::memory_order_release);
  // Mirror to legacy store for tests that still read snapshot store, but ignore its result.
  bool snapOk = store_.tryPublish(draft_);
  if (snapOk) published_.store(true, std::memory_order_release);
  else published_.store(true, std::memory_order_release); // paired success still counts as published
  return true;
}

bool HomeSceneModel::publishLoading() {
  begin(UiScene::DataState::Loading);
  return publish();
}

bool HomeSceneModel::publishEmpty(uint8_t errorCode) {
  begin(UiScene::DataState::Empty);
  draft_.errorCode = errorCode;
  return publish();
}

bool HomeSceneModel::publishError(uint8_t errorCode) {
  begin(UiScene::DataState::Error);
  draft_.errorCode = errorCode;
  return publish();
}

bool HomeSceneModel::publishStale() {
  HomeScenePublication pub{};
  if (!copyLatestPublication(pub)) {
    HomeSceneSnapshot snapshot{};
    if (!copyLatest(snapshot)) return false;
    pub.snapshot = snapshot;
  }
  pub.snapshot.state = UiScene::DataState::Stale;
  pub.snapshot.stale = true;
  pub.snapshot.revision = nextRevision_++;
  bool pubOk = pubStore_.tryPublish(pub);
  if (!pubOk) return false;
  pubPublished_.store(true, std::memory_order_release);
  bool snapOk = store_.tryPublish(pub.snapshot);
  if (snapOk) published_.store(true, std::memory_order_release);
  else published_.store(true, std::memory_order_release);
  return true;
}

bool HomeSceneModel::setBattery(int32_t percent) {
  draft_.battery = percent;
  return true;
}

bool HomeSceneModel::setWifiConnected(bool connected) {
  draft_.wifiConnected = connected;
  return true;
}

bool HomeSceneModel::setBrandText(const char* text) {
  if (!text || text[0] == '\0') text = "Murphy M4";
  const char* values[] = {text};
  if (!canAppend(values, 1)) return false;
  draft_.brandText = appendText(text);
  return true;
}

bool HomeSceneModel::setSelectedIndex(uint8_t index) {
  draft_.selectedIndex = index;
  return true;
}

bool HomeSceneModel::setCurrent(const char* title, const char* author,
                                const char* source, const char* cover,
                                int32_t progress) {
  char progressText[16]{};
  const int written = std::snprintf(progressText, sizeof(progressText), "%ld%%",
                                    static_cast<long>(progress));
  if (written < 0 || static_cast<std::size_t>(written) >= sizeof(progressText))
    return false;
  const char* values[] = {title, author, source, cover, progressText};
  if (!canAppend(values, 5)) return false;
  draft_.currentTitle = appendText(title);
  draft_.currentAuthor = appendText(author);
  draft_.currentSource = appendText(source);
  draft_.currentCover = appendText(cover);
  draft_.currentProgressText = appendText(progressText);
  draft_.currentProgress = progress;
  draft_.currentExists = title && title[0] != '\0';
  return true;
}

bool HomeSceneModel::setCurrentPaths(const char* path, const char* originalSource) {
  const char* values[] = {path, originalSource};
  if (!canAppend(values, 2)) return false;
  draft_.currentPath = appendText(path);
  draft_.currentOriginalSource = appendText(originalSource);
  return true;
}

bool HomeSceneModel::addRecent(const char* title, const char* author,
                               const char* source, const char* cover,
                               int32_t progress) {
  if (draft_.recentCount >= kMaxRecentItems) return false;
  const char* values[] = {title, author, source, cover};
  if (!canAppend(values, 4)) return false;
  HomeRecentItem& item = draft_.recent[draft_.recentCount++];
  item.title = appendText(title);
  item.author = appendText(author);
  item.source = appendText(source);
  item.cover = appendText(cover);
  item.progress = progress;
  return true;
}

bool HomeSceneModel::setRecentPaths(uint8_t index, const char* path,
                                    const char* originalSource) {
  if (index >= draft_.recentCount) return false;
  const char* values[] = {path, originalSource};
  if (!canAppend(values, 2)) return false;
  draft_.recent[index].path = appendText(path);
  draft_.recent[index].originalSource = appendText(originalSource);
  return true;
}

bool HomeSceneModel::addApp(const char* id, const char* name, const char* icon) {
  if (draft_.appCount >= kMaxAppItems) return false;
  const char* values[] = {id, name, icon};
  if (!canAppend(values, 3)) return false;
  HomeAppItem& item = draft_.apps[draft_.appCount++];
  item.id = appendText(id);
  item.name = appendText(name);
  item.icon = appendText(icon);
  return true;
}

bool HomeSceneModel::copyLatest(HomeSceneSnapshot& out) const {
  // Prefer publication store if it has been published (authoritative paired generation).
  if (pubPublished_.load(std::memory_order_acquire)) {
    HomeScenePublication pub{};
    if (copyLatestPublication(pub)) {
      out = pub.snapshot;
      return true;
    }
  }
  if (!published_.load(std::memory_order_acquire)) return false;
  auto snapshot = store_.acquire();
  if (!snapshot.valid()) return false;
  out = snapshot.value();
  return true;
}

bool HomeSceneModel::hasPublishedSnapshot() const {
  return published_.load(std::memory_order_acquire) || pubPublished_.load(std::memory_order_acquire);
}

uint32_t HomeSceneModel::latestRevision() const {
  HomeSceneSnapshot snapshot{};
  return copyLatest(snapshot) ? snapshot.revision : 0;
}

bool HomeSceneModel::publishWithAssets(const HomeScenePublication& pub) {
  HomeScenePublication copy = pub;
  copy.snapshot.revision = nextRevision_++;
  // Keep draft_ in sync for legacy copyLatest fallback
  draft_ = copy.snapshot;
  *draftPubPtr_ = copy;
  bool pubOk = pubStore_.tryPublish(copy);
  if (!pubOk) return false;
  pubPublished_.store(true, std::memory_order_release);
  bool snapOk = store_.tryPublish(copy.snapshot);
  if (snapOk) published_.store(true, std::memory_order_release);
  else published_.store(true, std::memory_order_release);
  return true;
}

bool HomeSceneModel::copyLatestPublication(HomeScenePublication& out) const {
  if (!pubPublished_.load(std::memory_order_acquire)) return false;
  auto snap = const_cast<UiStateStore<HomeScenePublication>&>(pubStore_).acquire();
  if (!snap.valid()) return false;
  out = snap.value();
  return true;
}

UiStateStore<HomeScenePublication>::Snapshot HomeSceneModel::acquirePublication() {
  return pubStore_.acquire();
}

bool HomeSceneModel::addPublicationAsset(const UiScene::AssetKey& key, const uint8_t* data,
                                         uint16_t w, uint16_t h, uint16_t stride) {
  return homeAddAssetToPublication(*draftPubPtr_, key, data, w, h, stride);
}

UiSceneRuntime::SceneBindingSource HomeSceneModel::bindingSource(
    const HomeSceneSnapshot& snapshot) {
  return {&snapshot, &HomeSceneModel::resolve, &HomeSceneModel::count};
}

bool HomeSceneModel::resolve(const void* user, UiScene::BindingId binding,
                             const UiSceneRuntime::SceneItemContext* item,
                             UiSceneRuntime::ResolvedValue* out) {
  if (!user || !out) return false;
  const auto& snapshot = *static_cast<const HomeSceneSnapshot*>(user);
  *out = UiSceneRuntime::ResolvedValue{};
  if (binding == kBindingBrandText) {
    out->kind = UiSceneRuntime::ValueKind::Text;
    out->text = snapshot.textView(snapshot.brandText);
    return out->text.data != nullptr;
  }
  if (binding == kBindingSystemBattery) {
    out->kind = UiSceneRuntime::ValueKind::Int;
    out->number = snapshot.battery;
    return true;
  }
  if (binding == kBindingWifiConnected || binding == kBindingCurrentExists) {
    out->kind = UiSceneRuntime::ValueKind::Bool;
    out->boolean = binding == kBindingWifiConnected ? snapshot.wifiConnected
                                                    : snapshot.currentExists;
    return true;
  }
  if (binding == kBindingCurrentProgress) {
    out->kind = UiSceneRuntime::ValueKind::Int;
    out->number = snapshot.currentProgress;
    return true;
  }
  if (binding == kBindingCurrentProgressText) {
    out->kind = UiSceneRuntime::ValueKind::Text;
    out->text = snapshot.textView(snapshot.currentProgressText);
    return out->text.data != nullptr;
  }
  if (binding == kBindingCurrentTitle || binding == kBindingCurrentAuthor ||
      binding == kBindingCurrentSource || binding == kBindingCurrentCover) {
    out->kind = UiSceneRuntime::ValueKind::Text;
    out->text = snapshot.textView(
        binding == kBindingCurrentTitle
            ? snapshot.currentTitle
            : binding == kBindingCurrentAuthor
                  ? snapshot.currentAuthor
                  : binding == kBindingCurrentSource ? snapshot.currentSource
                                                     : snapshot.currentCover);
    return out->text.data != nullptr;
  }
  if (!item || !item->valid) return false;
  if (item->sourceBinding == kBindingRecent && item->index < snapshot.recentCount) {
    const auto& recent = snapshot.recent[item->index];
    HomeTextRef ref{};
    if (binding == kBindingItemTitle) ref = recent.title;
    else if (binding == kBindingItemCover) ref = recent.cover;
    else if (binding == kBindingItemProgress) {
      out->kind = UiSceneRuntime::ValueKind::Int;
      out->number = recent.progress;
      return true;
    } else return false;
    out->kind = UiSceneRuntime::ValueKind::Text;
    out->text = snapshot.textView(ref);
    return out->text.data != nullptr;
  }
  if (item->sourceBinding == kBindingApps && item->index < snapshot.appCount) {
    const auto& app = snapshot.apps[item->index];
    HomeTextRef ref{};
    if (binding == kBindingItemId) ref = app.id;
    else if (binding == kBindingItemName) ref = app.name;
    else if (binding == kBindingItemIcon) ref = app.icon;
    else return false;
    out->kind = UiSceneRuntime::ValueKind::Text;
    out->text = snapshot.textView(ref);
    return out->text.data != nullptr;
  }
  return false;
}

uint8_t HomeSceneModel::count(const void* user, UiScene::BindingId source) {
  if (!user) return 0;
  const auto& snapshot = *static_cast<const HomeSceneSnapshot*>(user);
  if (source == kBindingRecent) return snapshot.recentCount;
  if (source == kBindingApps) return snapshot.appCount;
  return 0;
}

bool HomeSceneModel::actionTarget(const HomeSceneSnapshot& snapshot,
                                  UiScene::ActionId action,
                                  const UiSceneRuntime::SceneItemContext* item,
                                  HomeSceneActionTarget* out) {
  if (!out) return false;
  *out = HomeSceneActionTarget{};
  out->action = action;
  if (action == kActionOpenCurrentBook) {
    if (item && item->valid && item->sourceBinding == kBindingRecent &&
        item->index < snapshot.recentCount) {
      const HomeRecentItem& recent = snapshot.recent[item->index];
      if (recent.path.length == 0) return false;
      out->itemIndex = item->index;
      return true;
    }
    return snapshot.currentExists;
  }
  if (action == kActionOpenHistory) return true;
  if (action == kActionOpenApps) return true;
  if (action == kActionOpenApp && item && item->valid &&
      item->sourceBinding == kBindingApps && item->index < snapshot.appCount) {
    out->itemIndex = item->index;
    const auto text = snapshot.textView(snapshot.apps[item->index].id);
    out->argumentLength = text.size < kMaxActionArgumentBytes
                              ? static_cast<uint8_t>(text.size)
                              : static_cast<uint8_t>(kMaxActionArgumentBytes);
    for (uint8_t i = 0; i < out->argumentLength; ++i) {
      out->argument[i] = static_cast<char>(text.readByte(i));
    }
    return true;
  }
  return false;
}

bool HomeSceneModel::actionTarget(UiScene::ActionId action,
                                  const UiSceneRuntime::SceneItemContext* item,
                                  HomeSceneActionTarget* out) const {
  HomeSceneSnapshot snapshot{};
  return copyLatest(snapshot) && actionTarget(snapshot, action, item, out);
}

}  // namespace HomeScene
