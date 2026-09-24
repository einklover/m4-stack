#include "util/M4ReturnCache.h"

#include "apps/providers/M4Psram.h"

#include <cstring>
#include <mutex>

namespace M4ReturnCache {
namespace {

std::mutex gHomeMu;
HomeScene::HomeScenePublication* gHome = nullptr;
bool gHomeValid = false;

std::mutex gDrawerMu;
bool gDrawerValid = false;
std::vector<M4xInstalledApp> gApps;
std::vector<DrawerItem> gItems;

struct IconBlob {
  std::string key;
  uint8_t* data = nullptr;
  size_t size = 0;
};

std::vector<IconBlob> gIcons;

std::string iconKey(const std::string& id, int versionCode, const std::string& path, const std::string& icon,
                     uint32_t installedAt) {
  std::string key;
  key.reserve(id.size() + path.size() + icon.size() + 24);
  key.append(id);
  key.push_back('\n');
  key.append(std::to_string(versionCode));
  key.push_back('\n');
  key.append(std::to_string(installedAt));
  key.push_back('\n');
  key.append(path);
  key.push_back('\n');
  key.append(icon);
  return key;
}

void clearIconsLocked() {
  for (auto& blob : gIcons) M4Psram::freePrefer(blob.data);
  gIcons.clear();
}

bool textRefEq(const HomeScene::HomeTextRef& a, const HomeScene::HomeTextRef& b) {
  return a.offset == b.offset && a.length == b.length;
}

bool snapshotVisibleEq(const HomeScene::HomeSceneSnapshot& a, const HomeScene::HomeSceneSnapshot& b) {
  if (a.state != b.state || a.errorCode != b.errorCode || a.stale != b.stale || a.wifiConnected != b.wifiConnected ||
      a.currentExists != b.currentExists || a.recentCount != b.recentCount || a.appCount != b.appCount ||
      a.battery != b.battery || a.currentProgress != b.currentProgress || a.textUsed != b.textUsed) {
    return false;
  }
  if (a.textUsed != 0 && std::memcmp(a.text, b.text, a.textUsed) != 0) return false;
  if (!textRefEq(a.currentProgressText, b.currentProgressText) || !textRefEq(a.currentTitle, b.currentTitle) ||
      !textRefEq(a.currentAuthor, b.currentAuthor) || !textRefEq(a.currentSource, b.currentSource) ||
      !textRefEq(a.currentCover, b.currentCover) || !textRefEq(a.currentPath, b.currentPath) ||
      !textRefEq(a.currentOriginalSource, b.currentOriginalSource)) {
    return false;
  }
  for (uint8_t i = 0; i < a.recentCount && i < HomeScene::kMaxRecentItems; ++i) {
    const auto& ra = a.recent[i];
    const auto& rb = b.recent[i];
    if (ra.progress != rb.progress || !textRefEq(ra.path, rb.path) || !textRefEq(ra.originalSource, rb.originalSource) ||
        !textRefEq(ra.title, rb.title) || !textRefEq(ra.author, rb.author) || !textRefEq(ra.source, rb.source) ||
        !textRefEq(ra.cover, rb.cover)) {
      return false;
    }
  }
  for (uint8_t i = 0; i < a.appCount && i < HomeScene::kMaxAppItems; ++i) {
    const auto& aa = a.apps[i];
    const auto& ab = b.apps[i];
    if (!textRefEq(aa.id, ab.id) || !textRefEq(aa.name, ab.name) || !textRefEq(aa.icon, ab.icon)) return false;
  }
  return true;
}

bool assetsEq(const HomeScene::HomeScenePublication& a, const HomeScene::HomeScenePublication& b) {
  if (a.assetCount != b.assetCount) return false;
  for (uint8_t i = 0; i < a.assetCount && i < HomeScene::kMaxHomeAssets; ++i) {
    const auto& ea = a.entries[i];
    const auto& eb = b.entries[i];
    if (ea.valid != eb.valid || ea.key != eb.key || ea.width != eb.width || ea.height != eb.height ||
        ea.stride != eb.stride || ea.offset != eb.offset) {
      return false;
    }
    const size_t bytes = static_cast<size_t>(ea.stride) * ea.height;
    if (static_cast<size_t>(ea.offset) + bytes > HomeScene::kHomeAssetArenaBytes) return false;
    if (bytes != 0 && std::memcmp(a.arena + ea.offset, b.arena + eb.offset, bytes) != 0) return false;
  }
  return true;
}

bool visibleEq(const HomeScene::HomeScenePublication& a, const HomeScene::HomeScenePublication& b) {
  return snapshotVisibleEq(a.snapshot, b.snapshot) && assetsEq(a, b);
}

bool keepable(const HomeScene::HomeScenePublication& publication) {
  const auto state = publication.snapshot.state;
  return state == UiScene::DataState::Ready || state == UiScene::DataState::Empty;
}

HomeScene::HomeScenePublication* homeSlot() {
  if (!gHome) {
    void* memory = M4Psram::mallocPrefer(sizeof(HomeScene::HomeScenePublication), "home-return");
    if (!memory) return nullptr;
    gHome = static_cast<HomeScene::HomeScenePublication*>(memory);
    *gHome = HomeScene::HomeScenePublication{};
  }
  return gHome;
}

void rememberIconsLocked(const std::vector<M4xInstalledApp>& apps, const std::vector<DrawerItem>& items) {
  clearIconsLocked();
  for (const auto& item : items) {
    if (!item.plugin || item.pluginIcon.empty() || item.appIndex < 0 ||
        item.appIndex >= static_cast<int>(apps.size())) {
      continue;
    }
    const auto& app = apps[static_cast<size_t>(item.appIndex)];
    auto* data = static_cast<uint8_t*>(M4Psram::mallocPrefer(item.pluginIcon.size(), "drawer-icon"));
    if (!data) continue;
    std::memcpy(data, item.pluginIcon.data(), item.pluginIcon.size());
    gIcons.push_back(IconBlob{iconKey(app.id, app.versionCode, app.path, app.icon, app.installedAt), data,
                              item.pluginIcon.size()});
  }
}

}  // namespace

bool seedHome(HomeScene::HomeSceneModel& model) {
  HomeScene::HomeScenePublication copy{};
  {
    std::lock_guard<std::mutex> lock(gHomeMu);
    if (!gHomeValid || !gHome) return false;
    copy = *gHome;
  }
  return model.publishWithAssets(copy);
}

void rememberHome(const HomeScene::HomeScenePublication& publication) {
  if (!keepable(publication)) return;
  std::lock_guard<std::mutex> lock(gHomeMu);
  auto* slot = homeSlot();
  if (!slot) return;
  *slot = publication;
  gHomeValid = true;
}

bool homeMatches(const HomeScene::HomeScenePublication& publication) {
  std::lock_guard<std::mutex> lock(gHomeMu);
  if (!gHomeValid || !gHome) return false;
  return visibleEq(*gHome, publication);
}

bool copyDrawerIcon(const std::string& id, int versionCode, const std::string& path, const std::string& icon,
                    uint32_t installedAt, std::vector<uint8_t>& out) {
  const std::string key = iconKey(id, versionCode, path, icon, installedAt);
  std::lock_guard<std::mutex> lock(gDrawerMu);
  for (const auto& blob : gIcons) {
    if (blob.key != key || !blob.data || blob.size == 0) continue;
    out.resize(blob.size);
    std::memcpy(out.data(), blob.data, blob.size);
    return true;
  }
  return false;
}

bool restoreDrawer(std::vector<M4xInstalledApp>& apps, std::vector<DrawerItem>& items) {
  std::lock_guard<std::mutex> lock(gDrawerMu);
  if (!gDrawerValid) return false;
  apps = gApps;
  items = gItems;
  for (auto& item : items) {
    if (!item.plugin || item.appIndex < 0 || item.appIndex >= static_cast<int>(apps.size())) continue;
    const auto& app = apps[static_cast<size_t>(item.appIndex)];
    const std::string key = iconKey(app.id, app.versionCode, app.path, app.icon, app.installedAt);
    for (const auto& blob : gIcons) {
      if (blob.key != key || !blob.data || blob.size == 0) continue;
      item.pluginIcon.assign(blob.data, blob.data + blob.size);
      break;
    }
  }
  return true;
}

void rememberDrawer(const std::vector<M4xInstalledApp>& apps, const std::vector<DrawerItem>& items) {
  std::lock_guard<std::mutex> lock(gDrawerMu);
  rememberIconsLocked(apps, items);
  gApps = apps;
  gItems = items;
  for (auto& item : gItems) {
    item.pluginIcon.clear();
    item.pluginIcon.shrink_to_fit();
  }
  gDrawerValid = true;
}

}  // namespace M4ReturnCache
