#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <SDCardManager.h>
#include <Txt.h>
#include <Utf8.h>
#include <Xtc.h>

#include <cstring>
#include <string>

#include <esp_heap_caps.h>
#include <vector>

#include <HalPowerManager.h>
#include "CrossPointSettings.h"
#include "I18n.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "apps/M4xRegistry.h"
#include "util/M4ContentProviderContract.h"
#include "util/M4HistoryReopen.h"
#include "util/M4HomeBookDetailMeta.h"
#include "BookmarkStore.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"
#include "util/StringUtils.h"
#include "util/HomeRef.h"
#include "util/TouchHitGeometry.h"
#include "util/HomeMofeiTemplateOverlay.h"
#include "ui/scene/GfxSceneRenderer.h"
#include "generated/mofei_classic_m4theme.h"
#include "generated/murphy_default_m4theme.h"
#include "components/themes/fengyan/FengyanTheme.h"
#include "activities/home/HomeSceneAssetDecoder.h"
#include "util/M4ProviderCoverCache.h"

namespace {

// Home owns the composition of the theme-owned cover and menu surfaces. Keep
// their geometry in one place so visual composition and touch hit-testing do
// not drift apart.
struct HomeCompositionLayout {
  int coverTop;
  int coverHeight;
  int menuTop;
  int menuHeight;
};

HomeCompositionLayout makeHomeCompositionLayout(const ThemeMetrics& metrics, int pageHeight) {
  if (UITheme::getInstance().getThemeType() == ThemeType::Fengyan) {
    return {HomeRef::Recent.y, HomeRef::Recent.h, HomeRef::Quick.y, HomeRef::Quick.h};
  }
  const int menuTop = metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.verticalSpacing;
  constexpr int kHomeFooterGapPx = 20;
  return {metrics.homeTopPadding, metrics.homeCoverTileHeight, menuTop,
          pageHeight - menuTop - metrics.buttonHintsHeight - kHomeFooterGapPx};
}

// A single sparse rule makes the transition from recent reading to actions
// legible without adding another card surface or a ghosting-prone fill.
constexpr int kHomeSectionRuleGapPx = 8;
constexpr int kHomeQuickColumns = 4;
constexpr int kHomeQuickHeaderOffset = 0;
#ifdef CROSSPOINT_MURPHY_M4
constexpr uint32_t kHomeSceneBackendStackBytes = 16 * 1024;
#endif

void drawHomeSectionRule(GfxRenderer& renderer, const ThemeMetrics& metrics,
                         const HomeCompositionLayout& layout, int pageWidth) {
  // HomeRef cards already outline themselves; keep the helper for contract tests.
  if (UITheme::getInstance().getThemeType() == ThemeType::Fengyan) return;
  const int ruleInset = metrics.contentSidePadding;
  const int ruleY = layout.menuTop - kHomeSectionRuleGapPx;
  if (pageWidth <= 0 || ruleInset < 0 || ruleInset * 2 >= pageWidth || ruleY < 0 ||
      ruleY >= renderer.getScreenHeight()) {
    return;
  }
  renderer.drawLine(ruleInset, ruleY, pageWidth - ruleInset - 1, ruleY, 1, true);
}

}  // namespace

#ifdef CROSSPOINT_MURPHY_M4

namespace {

std::string homeSceneText(const HomeScene::HomeSceneSnapshot& snapshot,
                          HomeScene::HomeTextRef ref) {
  const auto view = snapshot.textView(ref);
  std::string result;
  result.reserve(view.size);
  for (uint16_t i = 0; i < view.size; ++i) {
    result.push_back(static_cast<char>(view.readByte(i)));
  }
  return result;
}

}  // namespace

void HomeActivity::sceneBackendTaskTrampoline(void* param) {
  // Take the task's ownership once. Do not pass shared_ptr by value through the
  // Xtensa task call chain; the task-local owner keeps the context alive.
  std::unique_ptr<std::shared_ptr<BackendContext>> holder(
      static_cast<std::shared_ptr<BackendContext>*>(param));
  std::shared_ptr<BackendContext> ctx = holder ? std::move(*holder) : nullptr;
  holder.reset();
  if (!ctx) {
    Serial.printf("[%lu] [Home] backend task missing context\n", millis());
    vTaskDelete(nullptr);
    for (;;) vTaskDelay(portMAX_DELAY);
  }
  backendLoop(*ctx);
  ctx->exiting.store(true, std::memory_order_release);
  // FreeRTOS task deletion does not unwind C++ stack locals.
  ctx.reset();
  vTaskDelete(nullptr);
  for (;;) vTaskDelay(portMAX_DELAY);
}

[[noreturn]] void HomeActivity::sceneBackendTaskLoop() {
  // Legacy path kept for ODR but should not be used after refactor.
  // If reached, just self-delete without touching HomeActivity `this`.
  vTaskDelete(nullptr);
  for (;;) vTaskDelay(portMAX_DELAY);
}

void HomeActivity::backendLoop(BackendContext& ctx) {
  publishHomeSceneFromBackendCtx(ctx);
}

void HomeActivity::loadRecentBooksInto(BackendContext& ctx, int maxBooks) {
  {
    std::vector<M4HomeBookDetailMeta::InstalledPlugin> plugins;
    const auto apps = M4xRegistry::load();
    plugins.reserve(apps.size());
    for (const auto& app : apps) plugins.push_back({app.id, app.name, app.provider});
    M4HomeBookDetailMeta::setInstalledPlugins(std::move(plugins));
  }
  ctx.recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  ctx.recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));
  for (RecentBook book : books) {
    if (static_cast<int>(ctx.recentBooks.size()) >= maxBooks) break;
    if (M4ContentProvider::isHistoryUri(book.path.c_str())) {
      book.progress = loadBookProgress(book.originalSourcePath.empty() ? book.path : book.originalSourcePath);
      ctx.recentBooks.push_back(book);
      continue;
    }
    if (!SdMan.exists(book.path.c_str())) continue;
    book.progress = loadBookProgress(book.path);
    ctx.recentBooks.push_back(book);
  }
}

bool HomeActivity::tryEnsureCoverThumbInCtx(BackendContext& ctx, const std::string& coverBmpPath, int w, int h,
                                            const std::function<bool()>& cancelled) {
  if (coverBmpPath.empty()) return false;
  std::string thumb = UITheme::getCoverThumbPath(coverBmpPath, w, h);
  if (SdMan.exists(thumb.c_str())) return true;
  if (cancelled && cancelled()) return false;
  if (M4ProviderCoverCache::ensureSizedCoverFromSource(coverBmpPath, w, h, cancelled) &&
      SdMan.exists(thumb.c_str())) {
    return true;
  }
  for (const auto& b : ctx.recentBooks) {
    std::string expectedThumb = UITheme::getCoverThumbPath(b.coverBmpPath, w, h);
    if (expectedThumb != thumb) continue;
    if (StringUtils::checkFileExtension(b.path, ".epub")) {
      Epub epub(b.path, "/.crosspoint");
      epub.load(false, true);
      if (epub.generateThumbBmp(w, h)) return true;
    }
    break;
  }
  return SdMan.exists(thumb.c_str());
}

bool HomeActivity::publishHomeSceneWithAssetsCtx(BackendContext& ctx) {
  uint32_t epoch = ctx.epoch.load(std::memory_order_acquire);
  auto isCancelled = [&ctx, epoch]() -> bool {
    return ctx.cancelled.load(std::memory_order_acquire) ||
           ctx.epoch.load(std::memory_order_acquire) != epoch;
  };
  if (isCancelled()) return false;
  HomeScene::HomeScenePublication& draftPub = ctx.model.draftPublication();
  if (!ctx.recentBooks.empty()) {
    const RecentBook& cur = ctx.recentBooks.front();
    if (tryEnsureCoverThumbInCtx(ctx, cur.coverBmpPath, HomeScene::kHomeCurrentCoverW, HomeScene::kHomeCurrentCoverH,
                                 isCancelled)) {
      std::string thumb = UITheme::getCoverThumbPath(cur.coverBmpPath, HomeScene::kHomeCurrentCoverW, HomeScene::kHomeCurrentCoverH);
      UiScene::AssetKey key{HomeScene::kBindingCurrentCover, UiScene::kInvalidBindingId, UiScene::kInvalidAssetItemIndex};
      if (isCancelled()) return false;
      (void)HomeSceneAssetDecoder::decodeCoverForPublication(draftPub, thumb.c_str(), key, isCancelled);
    }
  }
  if (isCancelled()) return false;
  uint8_t itemIndex = 0;
  for (size_t i = 1; i < ctx.recentBooks.size() && itemIndex < 3; ++i) {
    if (isCancelled()) return false;
    const RecentBook& b = ctx.recentBooks[i];
    if (tryEnsureCoverThumbInCtx(ctx, b.coverBmpPath, HomeScene::kHomeRecentCoverW, HomeScene::kHomeRecentCoverH,
                                 isCancelled)) {
      std::string thumb = UITheme::getCoverThumbPath(b.coverBmpPath, HomeScene::kHomeRecentCoverW, HomeScene::kHomeRecentCoverH);
      UiScene::AssetKey key{HomeScene::kBindingItemCover, HomeScene::kBindingRecent, itemIndex};
      (void)HomeSceneAssetDecoder::decodeCoverForPublication(draftPub, thumb.c_str(), key, isCancelled);
    }
    itemIndex++;
    if (isCancelled()) return false;
  }
  const auto apps = M4xRegistry::load();
  // addApp wrote draft_, not draftPub.snapshot (that copy happens in publish()).
  draftPub.snapshot = ctx.model.draftSnapshot();
  // Decode in snapshot order (builtin.files first, then preferred plugins).
  {
    const auto& snap = draftPub.snapshot;
    for (uint8_t i = 0; i < snap.appCount; ++i) {
      if (isCancelled()) return false;
      // Extract app id from snapshot text arena.
      std::string appId;
      {
        auto view = snap.textView(snap.apps[i].id);
        appId.reserve(view.size);
        for (uint16_t k = 0; k < view.size; ++k) appId.push_back(static_cast<char>(view.readByte(k)));
      }
      UiScene::AssetKey key{HomeScene::kBindingItemIcon, HomeScene::kBindingApps, i};
      if (appId == "builtin.files") {
        (void)HomeSceneAssetDecoder::decodeBuiltinFilesIconForPublication(draftPub, key, isCancelled);
      } else {
        const auto* found = M4xRegistry::find(apps, appId);
        if (found) {
          (void)HomeSceneAssetDecoder::decodeAppIconForPublication(draftPub, found->path, found->icon, key, isCancelled);
        }
      }
      if (isCancelled()) return false;
    }
  }
  if (isCancelled()) return false;
  if (isCancelled()) return false;
  if (ctx.model.publish()) {
    ctx.updateRequired.store(true, std::memory_order_release);
    return true;
  }
  return false;
}

void HomeActivity::publishHomeSceneFromBackendCtx(BackendContext& ctx) {
  const auto metrics = UITheme::getInstance().getMetrics();
  uint32_t epoch = ctx.epoch.load(std::memory_order_acquire);
  auto isCancelled = [&ctx, epoch]() -> bool {
    return ctx.cancelled.load(std::memory_order_acquire) ||
           ctx.epoch.load(std::memory_order_acquire) != epoch;
  };
  loadRecentBooksInto(ctx, metrics.homeRecentBooksCount);
  if (isCancelled()) return;
  ctx.model.begin(UiScene::DataState::Ready);
  ctx.model.setBattery(powerManager.getBatteryPercentage());
  ctx.model.setWifiConnected(false);
  if (!ctx.recentBooks.empty()) {
    const RecentBook& current = ctx.recentBooks.front();
    ctx.model.setCurrent(current.title.c_str(), current.author.c_str(), "", current.coverBmpPath.c_str(), current.progress);
    ctx.model.setCurrentPaths(current.path.c_str(), current.originalSourcePath.c_str());
  }
  uint8_t recentIndex = 0;
  // Mini recents skip books[0] (already the hero) so the strip shows 3 unique titles.
  for (size_t i = 0; i < ctx.recentBooks.size(); ++i) {
    if (isCancelled()) return;
    if (i == 0) continue;
    const RecentBook& book = ctx.recentBooks[i];
    if (ctx.model.addRecent(book.title.c_str(), book.author.c_str(), "", book.coverBmpPath.c_str(), book.progress)) {
      ctx.model.setRecentPaths(recentIndex++, book.path.c_str(), book.originalSourcePath.c_str());
    }
  }
  const auto apps = M4xRegistry::load();
  bool hasApps = false;
  // Dock order (must match mockup): 1. 文件管理 (builtin) 2. 微信读书 3. 番茄小说 4. 晋江文学
  // Always publish builtin.files at slot 0, then prefer weread/fanqie/jjwxc if installed,
  // fill remaining from registry without duplicating.
  if (ctx.model.addApp("builtin.files", "文件管理", "builtin.files")) {
    hasApps = true;
  }
  // Prefer the three mockup plugins in order.
  const char* kPreferredIds[3] = {"com.weread.client", "com.fanqie.client", "com.jjwxc.client"};
  for (int pi = 0; pi < 3; ++pi) {
    if (isCancelled()) return;
    const auto* found = M4xRegistry::find(apps, kPreferredIds[pi]);
    if (found) {
      // Avoid duplicate if somehow already added (should not happen for builtin)
      bool already = false;
      // Simple check: if id already in model, skip (model has no lookup, so we rely on registry not containing builtin)
      if (found->id == "builtin.files") already = true;
      if (already) continue;
      if (!ctx.model.addApp(found->id.c_str(), found->name.c_str(), found->icon.c_str())) break;
      hasApps = true;
    }
  }
  // Fill remaining slots from registry in load order, skipping duplicates and preferred already handled.
  for (const auto& app : apps) {
    if (isCancelled()) return;
    if (app.id == "builtin.files") continue;
    bool isPreferred = false;
    for (int pi = 0; pi < 3; ++pi) if (app.id == kPreferredIds[pi]) { isPreferred = true; break; }
    if (isPreferred) continue; // already considered
    if (!ctx.model.addApp(app.id.c_str(), app.name.c_str(), app.icon.c_str())) break;
    hasApps = true;
  }
  if (UITheme::getInstance().getThemeType() == ThemeType::Fengyan && !hasApps) {
    hasApps = ctx.model.addApp("com.weread.client", "微信读书", "book");
    hasApps = ctx.model.addApp("com.fanqie.client", "番茄", "tomato") || hasApps;
    hasApps = ctx.model.addApp("com.jjwxc.client", "晋江", "library") || hasApps;
  }
  if (ctx.recentBooks.empty() && !hasApps) ctx.model.begin(UiScene::DataState::Empty);
  if (isCancelled()) return;
  (void)publishHomeSceneWithAssetsCtx(ctx);
}

// Legacy wrappers kept for non-refactored call sites (should not be used in M4 path).
bool HomeActivity::tryEnsureCoverThumb(const std::string& coverBmpPath, int w, int h) {
  if (coverBmpPath.empty()) return false;
  std::string thumb = UITheme::getCoverThumbPath(coverBmpPath, w, h);
  if (SdMan.exists(thumb.c_str())) return true;
  if (M4ProviderCoverCache::ensureSizedCoverFromSource(coverBmpPath, w, h) && SdMan.exists(thumb.c_str())) {
    return true;
  }
  for (const auto& b : recentBooks) {
    std::string expectedThumb = UITheme::getCoverThumbPath(b.coverBmpPath, w, h);
    if (expectedThumb != thumb) continue;
    if (StringUtils::checkFileExtension(b.path, ".epub")) {
      Epub epub(b.path, "/.crosspoint");
      epub.load(false, true);
      if (epub.generateThumbBmp(w, h)) return true;
    }
    break;
  }
  return SdMan.exists(thumb.c_str());
}

bool HomeActivity::publishHomeSceneWithAssets() {
  if (!backendCtx) return false;
  return publishHomeSceneWithAssetsCtx(*backendCtx);
}

void HomeActivity::publishHomeSceneFromBackend() {
  if (!backendCtx) return;
  publishHomeSceneFromBackendCtx(*backendCtx);
}

bool HomeActivity::queueHomeSceneAction(const UiScene::UiSceneAction& action) {
  return sceneActionQueue.tryEnqueue(action);
}

bool HomeActivity::dispatchHomeSceneAction(
    void* user, const UiScene::UiSceneAction& action) {
  return static_cast<HomeActivity*>(user)->dispatchHomeSceneAction(action);
}

bool HomeActivity::dispatchHomeSceneAction(
    const UiScene::UiSceneAction& action) {
  if (!backendCtx) return false;
  HomeScene::HomeSceneSnapshot snapshot{};
  if (!backendCtx->model.copyLatest(snapshot)) return false;
  if (action.action == HomeScene::kActionOpenCurrentBook) {
    if (action.itemIndex < snapshot.recentCount) {
      const auto& recent = snapshot.recent[action.itemIndex];
      onSelectBook(homeSceneText(snapshot, recent.path),
                   homeSceneText(snapshot, recent.originalSource));
    } else {
      onSelectBook(homeSceneText(snapshot, snapshot.currentPath),
                   homeSceneText(snapshot, snapshot.currentOriginalSource));
    }
  } else if (action.action == HomeScene::kActionOpenHistory) {
    onRecentsOpen();
  } else if (action.action == HomeScene::kActionOpenApps) {
    onAppsOpen();
  } else if (action.action == HomeScene::kActionOpenApp &&
             action.itemIndex < snapshot.appCount) {
    if (onOpenNativeApp) {
      onOpenNativeApp(homeSceneText(snapshot, snapshot.apps[action.itemIndex].id));
    }
  }
  return true;
}

void HomeActivity::dispatchHomeSceneActions() {
  sceneActionDispatcher.dispatchAvailable(sceneActionQueue,
                                           &HomeActivity::dispatchHomeSceneAction,
                                           this, 1);
}

void HomeActivity::handleSnapshotInput() {
  // Global Home is handled before Activity::loop(). Back is also consumed
  // before page-action dispatch so navigation cannot wait on a content action.
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) return;
  if (!backendCtx) return;
  HomeScene::HomeSceneSnapshot snapshot{};
  if (!backendCtx->model.copyLatest(snapshot)) return;
  const auto source = HomeScene::HomeSceneModel::bindingSource(snapshot);

  // Home is the root activity; consume Back locally without touching the
  // backend. Global Home gestures are handled by main.cpp before this loop.
  const int focusCount = snapshot.recentCount + snapshot.appCount;
  if (focusCount > 0) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      sceneFocusIndex = static_cast<uint8_t>((sceneFocusIndex + focusCount - 1) % focusCount);
      if (backendCtx) backendCtx->updateRequired.store(true, std::memory_order_release);
      else updateRequired.store(true, std::memory_order_release);
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
        mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      sceneFocusIndex = static_cast<uint8_t>((sceneFocusIndex + 1) % focusCount);
      if (backendCtx) backendCtx->updateRequired.store(true, std::memory_order_release);
      else updateRequired.store(true, std::memory_order_release);
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      HomeScene::HomeSceneActionTarget action{};
      UiSceneRuntime::SceneItemContext item{};
      UiScene::ActionId actionId = HomeScene::kActionOpenCurrentBook;
      if (sceneFocusIndex < snapshot.recentCount) {
        item = {true, HomeScene::kBindingRecent, sceneFocusIndex,
                snapshot.recentCount};
      } else {
        actionId = HomeScene::kActionOpenApp;
        item = {true, HomeScene::kBindingApps,
                static_cast<uint8_t>(sceneFocusIndex - snapshot.recentCount),
                snapshot.appCount};
      }
      if (HomeScene::HomeSceneModel::actionTarget(snapshot, actionId, &item,
                                                   &action)) {
        queueHomeSceneAction(action);
      }
      return;
    }
  }
  int touchX = 0;
  int touchY = 0;
  if (!mappedInput.hasTouch() || !mappedInput.wasScreenTapped(touchX, touchY)) return;

  UiSceneRuntime::HitResult hit{};
  if (!UiSceneRuntime::hitTestScene(murphy_default_m4theme,
                                    murphy_default_m4theme_len, source, touchX,
                                    touchY, &hit) || !hit.hit) return;
  HomeScene::HomeSceneActionTarget action{};
  if (HomeScene::HomeSceneModel::actionTarget(snapshot, hit.action, &hit.item,
                                               &action)) {
    queueHomeSceneAction(action);
  }
}

void HomeActivity::renderSnapshotScene() {
  if (!backendCtx) {
    renderer.clearScreen();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }
  // Pin a stable publication generation for the entire render + displayBuffer submission.
  // This keeps asset arena pointers valid for the whole frame even if backend publishes next frame.
  auto pinned = backendCtx->model.acquirePublication();
  if (!pinned.valid()) {
    renderer.clearScreen();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }
  const HomeScene::HomeScenePublication& pub = pinned.value();
  // Build immutable assets view pointing into the pinned arena — still backend-free.
  UiScene::UiSceneAssets assets;
  HomeScene::homePublicationToAssets(pub, assets);
  HomeScene::HomeSceneSnapshot snapshot = pub.snapshot;
  snapshot.selectedIndex = sceneFocusIndex;
  const auto source = HomeScene::HomeSceneModel::bindingSource(snapshot);
  UiScene::GfxSceneRenderer sceneRenderer;
  // Pure render: only reads snapshot + assets arena + package + framebuffer. No SD/Bitmap/JSON.
  sceneRenderer.render(murphy_default_m4theme, murphy_default_m4theme_len, source, assets, renderer);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  (void)renderer.storeLastShown();
  // pinned is released after displayBuffer, keeping generation stable throughout.
}

#endif

void HomeActivity::taskTrampoline(void* param) {
  auto* self = static_cast<HomeActivity*>(param);
  self->displayTaskLoop();
}

int HomeActivity::getMenuItemCount() const {
#ifdef CROSSPOINT_MURPHY_M4
  const int recents = backendCtx ? static_cast<int>(backendCtx->recentBooks.size()) : static_cast<int>(recentBooks.size());
#else
  const int recents = static_cast<int>(recentBooks.size());
#endif
  if (UITheme::getInstance().getThemeType() == ThemeType::Fengyan) {
    return recents + 4;
  }
  int count = 5;  // My Library, Recents, File transfer, Apps, Settings
  if (!recentBooks.empty()) {
    count += recents;
  }
  if (hasOpdsUrl) {
    count++;
  }
  if (hasjianguoUrl) count++;
  if (hasDataCapsuleUrl) count++;  // 数据胶囊
  if (hasBookmarkNotes) count++;   // 书签笔记
  return count;

}


void HomeActivity::loadRecentBooks(int maxBooks) {
  {
    std::vector<M4HomeBookDetailMeta::InstalledPlugin> plugins;
    const auto apps = M4xRegistry::load();
    plugins.reserve(apps.size());
    for (const auto& app : apps) {
      plugins.push_back({app.id, app.name, app.provider});
    }
    M4HomeBookDetailMeta::setInstalledPlugins(std::move(plugins));
  }

  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));

  for (RecentBook book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    if (M4ContentProvider::isHistoryUri(book.path.c_str())) {
      // Keep provider entries for history reopen (cache or app launch).
      book.progress = loadBookProgress(book.originalSourcePath.empty() ? book.path : book.originalSourcePath);
      recentBooks.push_back(book);
      continue;
    }

    // Skip if file no longer exists
    if (!SdMan.exists(book.path.c_str())) {
      continue;
    }

    // 读取书籍进度
    book.progress = loadBookProgress(book.path);

    recentBooks.push_back(book);
  }
}

int HomeActivity::loadBookProgress(const std::string& path) {
  // 根据文件扩展名确定缓存路径和进度文件格式
  std::string cachePath;
  
  if (StringUtils::checkFileExtension(path, ".epub")) {
    // EPUB: 缓存路径在 /.crosspoint/epub_cache/{hash}/
    // 进度文件格式: spineIndex(2字节) + currentPage(2字节) + pageCount(2字节)
    Epub epub(path, "/.crosspoint");
    cachePath = epub.getCachePath();
    if (cachePath.empty()) return 0;
    
    std::string progressPath = cachePath + "/progress.bin";
    FsFile f;
    if (SdMan.openFileForRead("HAP", progressPath, f)) {
      uint8_t data[6];
      if (f.read(data, 6) == 6) {
        int spineIndex = data[0] | (data[1] << 8);
        int currentPage = data[2] | (data[3] << 8);
        int pageCount = data[4] | (data[5] << 8);
        f.close();
        
        // 计算总进度百分比
        if (epub.load(false, true)) {
          float chapterProgress = (pageCount > 0) ? (float)currentPage / pageCount : 0;
          float bookProgress = epub.calculateProgress(spineIndex, chapterProgress) * 100;
          return std::min(100, std::max(0, (int)(bookProgress + 0.5f)));
        }
      }
      f.close();
    }
  } else if (StringUtils::checkFileExtension(path, ".xtc") ||
             StringUtils::checkFileExtension(path, ".xtch")) {
    // XTC: 进度文件格式: currentPage(4字节) + m_loadedMax(4字节)
    Xtc xtc(path, "/.crosspoint");
    cachePath = xtc.getCachePath();
    if (cachePath.empty()) return 0;
    
    std::string progressPath = cachePath + "/progress.bin";
    FsFile f;
    if (SdMan.openFileForRead("HAP", progressPath, f)) {
      uint8_t data[8];
      if (f.read(data, 8) >= 4) {
        int currentPage = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
        f.close();
        
        // XTC 总页数需要从文件读取
        int totalPages = xtc.getPageCount();
        if (totalPages > 0) {
          return std::min(100, (currentPage * 100) / totalPages);
        }
      }
      f.close();
    }
  } else if (StringUtils::checkFileExtension(path, ".txt") ||
             StringUtils::checkFileExtension(path, ".md")) {
    // TXT progress: progress.dat (preferred) / progress.tmp / progress.bin
    // Format (8 bytes): page uint16 LE, pad, chapter uint16 LE, pad
    // (matches TxtReaderActivity::saveProgress / loadProgress)
    Txt txt(path, "/.crosspoint");
    if (!txt.load()) return 0;
    const size_t fileSize = txt.getFileSize();
    if (fileSize == 0) return 0;

    const std::string dir = txt.getCachePath();
    auto readProg = [&](const char* name, int& pageOut, int& chOut) -> bool {
      FsFile f;
      if (!SdMan.openFileForRead("HAP", (dir + name).c_str(), f)) return false;
      uint8_t data[8];
      const size_t n = f.read(data, 8);
      f.close();
      if (n != 8) return false;
      pageOut = data[0] | (data[1] << 8);
      chOut = data[4] | (data[5] << 8);
      return true;
    };

    int page = 0, ch = 0;
    if (!readProg("/progress.dat", page, ch) && !readProg("/progress.tmp", page, ch) &&
        !readProg("/progress.bin", page, ch)) {
      return 0;
    }
    if (ch < 0 || ch > 5000) return 0;
    if (page < 0) page = 0;

    // Prefer byte position from chapter batch cache (no full-file scan on home).
    const int batch = (ch / 25) * 25;
    size_t bytePos = 0;
    bool haveOffset = false;
    if (txt.hasChapterBatchCache(batch)) {
      txt.parseChapterIndexAndOffset(batch, /*allowScan=*/false);
      if (txt.isChapterExist(ch)) {
        const uint32_t begin = txt.getChapterOffsetByIndex(ch);
        uint32_t end = txt.getChapterendOffsetByIndex(ch);
        if (end == 0 || end <= begin) end = static_cast<uint32_t>(fileSize);
        // Within-chapter fraction from page index when available is ideal; without
        // parsing chapterN.bin headers, use a smooth page curve.
        float frac = 0.f;
        if (page > 0) {
          frac = std::min(0.95f, static_cast<float>(page) / (static_cast<float>(page) + 12.0f));
        }
        bytePos = begin + static_cast<size_t>((static_cast<double>(end - begin) * frac));
        haveOffset = true;
      }
    }
    if (!haveOffset) {
      // Fallback: chapter index vs highest cached batch (still no full scan).
      int maxCh = ch;
      for (int b = 0; b <= 2500; b += 25) {
        if (!txt.hasChapterBatchCache(b)) {
          if (b > batch) break;
          continue;
        }
        txt.parseChapterIndexAndOffset(b, /*allowScan=*/false);
        for (int i = 0; i < 25; ++i) {
          if (txt.isChapterExist(b + i)) maxCh = b + i;
        }
      }
      float p = static_cast<float>(ch) / static_cast<float>(maxCh + 1);
      p += (1.0f / static_cast<float>(maxCh + 1)) * std::min(0.9f, page / 30.0f);
      return std::min(100, std::max(0, static_cast<int>(p * 100.0f + 0.5f)));
    }
    if (bytePos >= fileSize) bytePos = fileSize - 1;
    return std::min(100, std::max(0, static_cast<int>(bytePos * 100.0 / static_cast<double>(fileSize) + 0.5)));
  }
  
  return 0;
}

void HomeActivity::loadRecentCovers(int coverWidth, int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  int progress = 0;
  for (RecentBook& book : recentBooks) {
    if (!book.coverBmpPath.empty()) {
      std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverWidth, coverHeight);
      if (!SdMan.exists(coverPath.c_str())) {
        // If epub, try to load the metadata for title/author and cover
        if (StringUtils::checkFileExtension(book.path, ".epub")) {
          {
            Epub epub(book.path, "/.crosspoint");
            // Skip loading css since we only need metadata here
            epub.load(false, true);

            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, "生成封面中...");
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
            bool success = epub.generateThumbBmp(coverWidth, coverHeight);
            if (!success) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
              book.coverBmpPath = "";
            }
          }  // Epub 对象在此析构，释放内存
        } else if (StringUtils::checkFileExtension(book.path, ".xtch") ||
                   StringUtils::checkFileExtension(book.path, ".xtc")) {
          // XTC files use default cover, no generation needed
          RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
          book.coverBmpPath = "";
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  // 强制竖屏（防止阅读器横屏后未正常退出导致首页横屏）
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  renderer.setRenderMode(GfxRenderer::BW);

  // Normal boot still uses the established HALF first paint. Returns from
  // another activity opt into the explicit bottom-to-top transition below.
  firstRenderDone = false;

  showMemWarning = false;
  memWarningSelected = false;

  // Low-memory warning: use the CURRENT internal heap state, not the
  // lifetime minimum (ESP.getMinFreeHeap() is the all-time low — a transient
  // 3.4KB dip during a chapter open would then keep warning long after the
  // heap recovered to 80KB+, a false positive). Warn only when free internal
  // RAM is currently critical or the largest free block can't serve a TLS
  // handshake (~40KB gate).
  const uint32_t freeInternal =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const uint32_t largestInternal =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  Serial.printf("[%lu] [Home] internal free=%lu largest=%lu lifetime_min=%lu\n", millis(),
                static_cast<unsigned long>(freeInternal),
                static_cast<unsigned long>(largestInternal),
                static_cast<unsigned long>(ESP.getMinFreeHeap()));
  if (freeInternal < 20 * 1024 || largestInternal < 12 * 1024) {
    showMemWarning = true;
    Serial.printf("[%lu] [Home] Low memory warning triggered (free=%lu largest=%lu)\n", millis(),
                  static_cast<unsigned long>(freeInternal),
                  static_cast<unsigned long>(largestInternal));
  }

  renderingMutex = xSemaphoreCreateMutex();

  // Check if OPDS browser URL is configured
  hasOpdsUrl = strlen(SETTINGS.opdsServerUrl) > 0;
  hasjianguoUrl = strlen(SETTINGS.jgUsername) > 0;
  hasDataCapsuleUrl = strlen(SETTINGS.dcUsername) > 0;  // 数据胶囊配置检查
  hasBookmarkNotes = BookmarkStore::hasAnyBookmarks();  // 书签笔记检查

  selectorIndex = 0;

#ifdef CROSSPOINT_MURPHY_M4
  sceneAssets.clear();
  sceneFocusIndex = 0;
  // Create independently owned backend context; backend task will hold a shared_ptr copy.
  // This keeps FsFile/Bitmap/Epub state off the Activity object so onExit can safely
  // detach without UAF, and no post-exit publish can touch a destroyed HomeActivity.
  backendCtx = std::make_shared<BackendContext>();
  backendCtx->epoch.fetch_add(1, std::memory_order_acq_rel);
  backendCtx->cancelled.store(false, std::memory_order_release);
  backendCtx->exiting.store(false, std::memory_order_release);
  backendCtx->updateRequired.store(false, std::memory_order_release);
  // Publish initial Loading via the context's model (PSRAM-backed arena).
  backendCtx->model.publishLoading();
  updateRequired.store(false, std::memory_order_release);
#else
  auto metrics = UITheme::getInstance().getMetrics();
  loadRecentBooks(metrics.homeRecentBooksCount);
#endif

  // Trigger first update
#ifdef CROSSPOINT_MURPHY_M4
  if (backendCtx) backendCtx->updateRequired.store(true, std::memory_order_release);
  else updateRequired.store(true, std::memory_order_release);

  // Capture task ownership before another task can run against HomeActivity.
  // The trampoline moves this owner into its task-local shared_ptr exactly once.
  auto* backendHolder = new std::shared_ptr<BackendContext>(backendCtx);
#else
  updateRequired.store(true, std::memory_order_release);
#endif

  if (xTaskCreate(&HomeActivity::taskTrampoline, "HomeActivityTask",
                  8192, this, 1, &displayTaskHandle) != pdPASS) {
    displayTaskHandle = nullptr;
    Serial.printf("[%lu] [Home] failed to create display task\n", millis());
  }
#ifdef CROSSPOINT_MURPHY_M4
  if (xTaskCreate(&HomeActivity::sceneBackendTaskTrampoline, "HomeSceneBackend",
                  kHomeSceneBackendStackBytes, backendHolder, 1,
                  &sceneBackendTaskHandle) != pdPASS) {
    delete backendHolder;
    sceneBackendTaskHandle = nullptr;
    Serial.printf("[%lu] [Home] failed to create backend task\n", millis());
  }
#endif
}

void HomeActivity::onExit() {
  Activity::onExit();

#ifdef CROSSPOINT_MURPHY_M4
  // Lifetime-safe cooperative join: signal backend via its own context and wait boundedly.
  // Backend owns its context via shared_ptr, so even if we return, it cannot touch `this`.
  // We never delete a task while it holds FsFile/Bitmap/Epub destructors.
  std::shared_ptr<BackendContext> ctx = backendCtx;
  if (ctx) {
    ctx->cancelled.store(true, std::memory_order_release);
    ctx->epoch.fetch_add(1, std::memory_order_acq_rel);
    if (sceneBackendTaskHandle) {
      const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(250);
      while (sceneBackendTaskHandle && !ctx->exiting.load(std::memory_order_acquire) && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(10));
      }
      if (sceneBackendTaskHandle && !ctx->exiting.load(std::memory_order_acquire)) {
        Serial.printf("[%lu] [Home] backend still alive after 250ms, not force-deleting (epoch blocked, ctx retained)\n", millis());
        sceneBackendTaskHandle = nullptr;
        // ctx stays alive via backend task's shared_ptr; our copy will be released below.
      } else if (sceneBackendTaskHandle) {
        // The backend task owns and deletes itself after setting exiting.
        sceneBackendTaskHandle = nullptr;
      }
    }
    // Release our reference; backend's copy keeps PSRAM arena alive if still running.
    // Any late publish will be ignored because epoch is bumped and cancelled is true,
    // and HomeActivity no longer reads from this ctx after we clear backendCtx.
    backendCtx.reset();
  } else if (sceneBackendTaskHandle) {
    // No context but task handle exists (should not happen) — just clear.
    vTaskDelete(sceneBackendTaskHandle);
    sceneBackendTaskHandle = nullptr;
  }
#endif

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  size_t xByteOffset = 0;
  size_t yStart = 0;
  size_t rowBytes = 0;
  size_t rows = 0;
  if (!computeCoverBufferLayout(xByteOffset, yStart, rowBytes, rows)) {
    return false;
  }

  const size_t bufferSize = rowBytes * rows;
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    return false;
  }

  // Free any existing buffer first
  freeCoverBuffer();

  coverBuffer = static_cast<uint8_t*>(malloc(bufferSize));
  if (!coverBuffer) {
    return false;
  }

  for (size_t row = 0; row < rows; ++row) {
    const size_t sourceOffset = (yStart + row) * HalDisplay::DISPLAY_WIDTH_BYTES + xByteOffset;
    memcpy(coverBuffer + row * rowBytes, frameBuffer + sourceOffset, rowBytes);
  }
  coverBufferXByteOffset = xByteOffset;
  coverBufferYStart = yStart;
  coverBufferRowBytes = rowBytes;
  coverBufferRows = rows;
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer) {
    return false;
  }

  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    return false;
  }

  if (coverBufferRowBytes == 0 || coverBufferRows == 0 ||
      coverBufferXByteOffset + coverBufferRowBytes > HalDisplay::DISPLAY_WIDTH_BYTES ||
      coverBufferYStart + coverBufferRows > HalDisplay::DISPLAY_HEIGHT) {
    return false;
  }

  for (size_t row = 0; row < coverBufferRows; ++row) {
    const size_t destOffset = (coverBufferYStart + row) * HalDisplay::DISPLAY_WIDTH_BYTES + coverBufferXByteOffset;
    memcpy(frameBuffer + destOffset, coverBuffer + row * coverBufferRowBytes, coverBufferRowBytes);
  }
  return true;
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferRowBytes = 0;
  coverBufferRows = 0;
  coverBufferXByteOffset = 0;
  coverBufferYStart = 0;
  coverBufferStored = false;
}

bool HomeActivity::computeCoverBufferLayout(size_t& xByteOffset, size_t& yStart, size_t& rowBytes, size_t& rows) const {
  const auto metrics = UITheme::getInstance().getMetrics();
  const int logicalStripY = metrics.homeTopPadding;
  const int logicalStripHeight = metrics.homeCoverTileHeight;
  if (logicalStripY < 0 || logicalStripHeight <= 0) {
    return false;
  }

  switch (renderer.getOrientation()) {
    case GfxRenderer::Portrait: {
      const int physicalX = logicalStripY;
      const int physicalWidth = logicalStripHeight;
      xByteOffset = physicalX / 8;
      yStart = 0;
      rowBytes = (physicalWidth + 7) / 8;
      rows = HalDisplay::DISPLAY_HEIGHT;
      break;
    }
    case GfxRenderer::PortraitInverted: {
      const int physicalX = HalDisplay::DISPLAY_WIDTH - logicalStripY - logicalStripHeight;
      const int physicalWidth = logicalStripHeight;
      xByteOffset = physicalX / 8;
      yStart = 0;
      rowBytes = (physicalWidth + 7) / 8;
      rows = HalDisplay::DISPLAY_HEIGHT;
      break;
    }
    case GfxRenderer::LandscapeCounterClockwise: {
      xByteOffset = 0;
      yStart = logicalStripY;
      rowBytes = HalDisplay::DISPLAY_WIDTH_BYTES;
      rows = logicalStripHeight;
      break;
    }
    case GfxRenderer::LandscapeClockwise: {
      xByteOffset = 0;
      yStart = HalDisplay::DISPLAY_HEIGHT - logicalStripY - logicalStripHeight;
      rowBytes = HalDisplay::DISPLAY_WIDTH_BYTES;
      rows = logicalStripHeight;
      break;
    }
  }

  if (xByteOffset >= HalDisplay::DISPLAY_WIDTH_BYTES || yStart >= HalDisplay::DISPLAY_HEIGHT || rowBytes == 0 ||
      rows == 0 || xByteOffset + rowBytes > HalDisplay::DISPLAY_WIDTH_BYTES || yStart + rows > HalDisplay::DISPLAY_HEIGHT) {
    return false;
  }

  return true;
}

void HomeActivity::loop() {
  // Handle low memory warning dialog first
  if (showMemWarning) {
    int touchX = 0;
    int touchY = 0;
    if (mappedInput.hasTouch() && mappedInput.wasScreenTapped(touchX, touchY)) {
      const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
      const int cancelWidth = M4UiText::textWidth(renderer, UI_12_FONT_ID, L(Str::kCancel));
      const int restartWidth = M4UiText::textWidth(renderer, UI_12_FONT_ID, L(Str::kRebootNow));
      constexpr int padding = 20;
      constexpr int btnSpacing = 30;
      const int btnRowWidth = cancelWidth + btnSpacing + restartWidth;
      const int boxH = lineHeight + 8 + lineHeight + 8 + lineHeight + padding * 3;
      const int boxY = (renderer.getScreenHeight() - boxH) / 2;
      const int btnY = boxY + boxH - padding - lineHeight;
      const int cancelX = (renderer.getScreenWidth() - btnRowWidth) / 2;
      const int restartX = cancelX + cancelWidth + btnSpacing;
      const TouchHitGeometry::Rect cancelRect{cancelX - 4, btnY - 2, cancelWidth + 8, lineHeight + 4};
      const TouchHitGeometry::Rect restartRect{restartX - 4, btnY - 2, restartWidth + 8, lineHeight + 4};
      if (cancelRect.contains(touchX, touchY)) {
        showMemWarning = false;
        updateRequired.store(true, std::memory_order_release);
      } else if (restartRect.contains(touchX, touchY)) {
        Serial.printf("[%lu] [Home] User confirmed restart due to low memory\n", millis());
        ESP.restart();
      }
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Left) ||
        mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      memWarningSelected = false;
      updateRequired.store(true, std::memory_order_release);
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Right) ||
               mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      memWarningSelected = true;
      updateRequired.store(true, std::memory_order_release);
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (memWarningSelected) {
        Serial.printf("[%lu] [Home] User confirmed restart due to low memory\n", millis());
        ESP.restart();
      } else {
        showMemWarning = false;
        updateRequired.store(true, std::memory_order_release);
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      showMemWarning = false;
      updateRequired.store(true, std::memory_order_release);
    }
    return;
  }

#ifdef CROSSPOINT_MURPHY_M4
  const bool backPressed = mappedInput.wasPressed(MappedInputManager::Button::Back);
  handleSnapshotInput();
  if (!backPressed) dispatchHomeSceneActions();
  return;
#else
  auto activateSelection = [this]() {
    int idx = 0;
    int menuSelectedIndex = selectorIndex - static_cast<int>(recentBooks.size());
    const bool isFengyanTheme = UITheme::getInstance().getThemeType() == ThemeType::Fengyan;

    if (selectorIndex < static_cast<int>(recentBooks.size())) {
      const auto& b = recentBooks[selectorIndex];
      const auto apps = M4xRegistry::load();
      const M4HistoryReopen::ProviderAppIdResolver appIdForProvider = [apps](const std::string& providerId) {
        std::string found;
        for (const auto& app : apps) {
          if (app.provider != providerId) continue;
          if (!found.empty() && found != app.id) return std::string();
          found = app.id;
        }
        return found;
      };
      const std::string src = M4HistoryReopen::appHintForRecentBook(
          b.path, b.originalSourcePath, b.author, appIdForProvider);
      onSelectBook(b.path, src);
      return;
    }

    if (isFengyanTheme) {
      if (menuSelectedIndex == 0) onMyLibraryOpen();
      else if (menuSelectedIndex == 1 && onOpenNativeApp) onOpenNativeApp("com.weread.client");
      else if (menuSelectedIndex == 2 && onOpenNativeApp) onOpenNativeApp("com.fanqie.client");
      else if (menuSelectedIndex == 3 && onOpenNativeApp) onOpenNativeApp("com.jjwxc.client");
      else if (menuSelectedIndex > 0) onAppsOpen();
      return;
    }

    const int myLibraryIdx = idx++;
    const int recentsIdx = idx++;
    const int opdsLibraryIdx = hasOpdsUrl ? idx++ : -1;
    const int jgLibraryIdx = hasjianguoUrl ? idx++ : -1;
    const int dcLibraryIdx = hasDataCapsuleUrl ? idx++ : -1;
    const int bookmarkNotesIdx = hasBookmarkNotes ? idx++ : -1;
    const int fileTransferIdx = idx++;
    const int appsIdx = idx++;
    const int settingsIdx = idx;

    if (menuSelectedIndex == myLibraryIdx) {
      onMyLibraryOpen();
    } else if (menuSelectedIndex == recentsIdx) {
      onRecentsOpen();
    } else if (menuSelectedIndex == opdsLibraryIdx) {
      onOpdsBrowserOpen();
    } else if (menuSelectedIndex == jgLibraryIdx) {
      onJianGuoYunOpen();
    } else if (menuSelectedIndex == dcLibraryIdx) {
      onDataCapsuleOpen();
    } else if (menuSelectedIndex == bookmarkNotesIdx) {
      onBookmarkNotesOpen();
    } else if (menuSelectedIndex == fileTransferIdx) {
      onFileTransferOpen();
    } else if (menuSelectedIndex == appsIdx) {
      onAppsOpen();
    } else if (menuSelectedIndex == settingsIdx) {
      onSettingsOpen();
    }
  };

  // Touch: shared geometry with render() for covers and menu tiles.
  if (mappedInput.hasTouch()) {
    const auto metrics = UITheme::getInstance().getMetrics();
    const int pageWidth = renderer.getScreenWidth();
    const int pageHeight = renderer.getScreenHeight();
    const auto homeLayout = makeHomeCompositionLayout(metrics, pageHeight);
    const TouchHitGeometry::Rect coverRect{0, homeLayout.coverTop, pageWidth, homeLayout.coverHeight};
    const TouchHitGeometry::Rect menuRect{
        0, homeLayout.menuTop, pageWidth, homeLayout.menuHeight};
    const int coverCount = static_cast<int>(recentBooks.size());
    const int menuCount = getMenuItemCount();
    const int renderedMenuCount = menuCount - coverCount;
    const bool isFengyanTheme = UITheme::getInstance().getThemeType() == ThemeType::Fengyan;

    int tx = 0;
    int ty = 0;
    int tapX = 0;
    int tapY = 0;
    const bool tapped = mappedInput.wasScreenTapped(tapX, tapY);
    if (isFengyanTheme && tapped) {
      int bottomHit = -1;
      if (TouchHitGeometry::fengyanHomeBottomIndexFromPoint(tapX, tapY, bottomHit, pageWidth, pageHeight)) {
        if (bottomHit == 0) onRecentsOpen();
        else if (bottomHit == 1) onAppsOpen();
        else if (bottomHit == 2) onSettingsOpen();
        return;
      }
    }
    if (mappedInput.wasScreenTouchDown(tx, ty) && !tapped) {
      int hit = -1;
      if (coverCount > 0 &&
          TouchHitGeometry::fengyanRecentBookIndexFromPoint(coverRect, coverCount, metrics.contentSidePadding,
                                                            tx, ty, hit)) {
        if (selectorIndex != hit) {
          selectorIndex = hit;
          updateRequired.store(true, std::memory_order_release);
        }
        return;
      }
      const bool menuHit = renderedMenuCount > 0 &&
          (isFengyanTheme
               ? TouchHitGeometry::fengyanMenuIndexFromPoint(menuRect, renderedMenuCount, tx, ty,
                                                              hit, kHomeQuickHeaderOffset,
                                                              metrics.contentSidePadding, kHomeQuickColumns)
               : TouchHitGeometry::lyraMenuIndexFromPoint(menuRect, renderedMenuCount, tx, ty, hit,
                                                           metrics.contentSidePadding, metrics.menuRowHeight,
                                                           metrics.menuSpacing));
      if (menuHit) {
        const int touched = coverCount + hit;
        if (selectorIndex != touched) {
          selectorIndex = touched;
          updateRequired.store(true, std::memory_order_release);
        }
        return;
      }
    }
    if (tapped) {
      tx = tapX;
      ty = tapY;
      int hit = -1;
      if (coverCount > 0 &&
          TouchHitGeometry::fengyanRecentBookIndexFromPoint(coverRect, coverCount, metrics.contentSidePadding,
                                                            tx, ty, hit)) {
        selectorIndex = hit;
        activateSelection();
        return;
      }
      const bool menuHit = renderedMenuCount > 0 &&
          (isFengyanTheme
               ? TouchHitGeometry::fengyanMenuIndexFromPoint(menuRect, renderedMenuCount, tx, ty,
                                                              hit, kHomeQuickHeaderOffset,
                                                              metrics.contentSidePadding, kHomeQuickColumns)
               : TouchHitGeometry::lyraMenuIndexFromPoint(menuRect, renderedMenuCount, tx, ty, hit,
                                                           metrics.contentSidePadding, metrics.menuRowHeight,
                                                           metrics.menuSpacing));
      if (menuHit) {
        selectorIndex = coverCount + hit;
        activateSelection();
        return;
      }
    }

    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Left) {
      selectorIndex = (selectorIndex + 1) % menuCount;
      updateRequired.store(true, std::memory_order_release);
      return;
    }
    if (swipe == MappedInputManager::SwipeDir::Down || swipe == MappedInputManager::SwipeDir::Right) {
      selectorIndex = (selectorIndex + menuCount - 1) % menuCount;
      updateRequired.store(true, std::memory_order_release);
      return;
    }
  }

  const bool prevPressed = mappedInput.wasPressed(MappedInputManager::Button::Up) ||
                           mappedInput.wasPressed(MappedInputManager::Button::Left);
  const bool nextPressed = mappedInput.wasPressed(MappedInputManager::Button::Down) ||
                           mappedInput.wasPressed(MappedInputManager::Button::Right);

  const int menuCount = getMenuItemCount();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelection();
  } else if (prevPressed) {
    selectorIndex = (selectorIndex + menuCount - 1) % menuCount;
    updateRequired.store(true, std::memory_order_release);
  } else if (nextPressed) {
    selectorIndex = (selectorIndex + 1) % menuCount;
    updateRequired.store(true, std::memory_order_release);
  }
#endif
}

void HomeActivity::displayTaskLoop() {
  while (true) {
#ifdef CROSSPOINT_MURPHY_M4
    bool need = false;
    if (backendCtx && backendCtx->updateRequired.exchange(false, std::memory_order_acq_rel)) need = true;
    if (updateRequired.exchange(false, std::memory_order_acq_rel)) need = true;
    if (need) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
#else
    if (updateRequired.exchange(false, std::memory_order_acq_rel)) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
#endif
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void HomeActivity::renderMemWarning() {
  renderer.clearScreen();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);

  const char* line1 = L(Str::kMemLowWarning);
  const char* line2 = L(Str::kMemLowReboot);
  const char* cancelText = L(Str::kCancel);
  const char* restartText = L(Str::kRebootNow);

  const int line1Width = M4UiText::textWidth(renderer, UI_12_FONT_ID, line1);
  const int line2Width = M4UiText::textWidth(renderer, UI_12_FONT_ID, line2);
  const int cancelWidth = M4UiText::textWidth(renderer, UI_12_FONT_ID, cancelText);
  const int restartWidth = M4UiText::textWidth(renderer, UI_12_FONT_ID, restartText);

  constexpr int padding = 20;
  constexpr int btnSpacing = 30;
  const int btnRowWidth = cancelWidth + btnSpacing + restartWidth;
  int contentWidth = line1Width;
  if (line2Width > contentWidth) contentWidth = line2Width;
  if (btnRowWidth > contentWidth) contentWidth = btnRowWidth;

  const int boxW = contentWidth + padding * 2;
  const int boxH = lineHeight + 8 + lineHeight + 8 + lineHeight + padding * 3;
  const int boxX = (pageWidth - boxW) / 2;
  const int boxY = (pageHeight - boxH) / 2;

  // Background and border
  renderer.fillRect(boxX - 3, boxY - 3, boxW + 6, boxH + 6, false);
  renderer.drawRect(boxX, boxY, boxW, boxH, true);

  // Message lines
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, boxY + padding, line1);
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, boxY + padding + lineHeight + 8, line2);

  // Buttons
  const int btnY = boxY + boxH - padding - lineHeight;
  const int cancelX = (pageWidth - btnRowWidth) / 2;
  const int restartX = cancelX + cancelWidth + btnSpacing;

  if (!memWarningSelected) {
    // "取消" selected
    renderer.fillRect(cancelX - 4, btnY - 2, cancelWidth + 8, lineHeight + 4, true);
    M4UiText::draw(renderer, UI_12_FONT_ID, cancelX, btnY, cancelText, false);
    M4UiText::draw(renderer, UI_12_FONT_ID, restartX, btnY, restartText, true);
  } else {
    // "立即重启" selected
    M4UiText::draw(renderer, UI_12_FONT_ID, cancelX, btnY, cancelText, true);
    renderer.fillRect(restartX - 4, btnY - 2, restartWidth + 8, lineHeight + 4, true);
    M4UiText::draw(renderer, UI_12_FONT_ID, restartX, btnY, restartText, false);
  }

  renderer.displayBuffer();
}

void HomeActivity::render() {
  // Show memory warning dialog if triggered
  if (showMemWarning) {
    renderMemWarning();
    return;
  }

#ifdef CROSSPOINT_MURPHY_M4
  renderSnapshotScene();
  return;
#else
  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto homeLayout = makeHomeCompositionLayout(metrics, pageHeight);
  const bool isFengyanTheme = (UITheme::getInstance().getThemeType() == ThemeType::Fengyan);
  if (isFengyanTheme) {
    // Template Home — black-ink overlay compositing (see HomeMofeiTemplateOverlay).
    // A) clear white, B) dynamic covers/text/progress/count, C) overlay template ink last, D) focus.
    // Template owns header/card/dividers/quick/footer ink — skip static repaints.
    renderer.clearScreen();
    auto* fengyan = static_cast<FengyanTheme*>(UITheme::getInstance().getTheme());
    if (fengyan) {
      fengyan->drawRecentBookCoverContent(renderer,
                                          Rect{0, homeLayout.coverTop, pageWidth, homeLayout.coverHeight},
                                          recentBooks);
    }
    HomeMofeiTemplateOverlay::draw(renderer, mofei_classic_m4theme, mofei_classic_m4theme_len);
    if (fengyan) {
      fengyan->drawRecentBookCoverFocus(renderer,
                                        Rect{0, homeLayout.coverTop, pageWidth, homeLayout.coverHeight},
                                        recentBooks, selectorIndex);
    }
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    (void)renderer.storeLastShown();
    return;
  }

  // Navigation surfaces use a single fast/partial frame. Reader-body page
  // pagination owns the only windowed animation path.
  const bool animateHomeEntry = animateEntry && !firstRenderDone;

  renderer.clearScreen();
  // Only restore the saved cover buffer when covers don't need re-rendering.
  // If coverRendered=false the saved buffer is stale (e.g. contains the "no cover" black block
  // from a previous render before the thumbnail was generated). Restoring it would pollute the
  // cleared white background, and since drawBitmap1Bit only draws black pixels (white pixels are
  // transparent), the old black fill would bleed through the light areas of the new thumbnail.
  const bool shouldRestoreBuffer = coverRendered && coverBufferStored;
  bool bufferRestored = shouldRestoreBuffer && restoreCoverBuffer();

  // Keep the home status bar clean; the legacy quote/custom-status-bar
  // feature has been removed, matching the original CrossLink home layout.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "主页");

  GUI.drawRecentBookCover(renderer, Rect{0, homeLayout.coverTop, pageWidth, homeLayout.coverHeight},
                          recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this));
  drawHomeSectionRule(renderer, metrics, homeLayout, pageWidth);

  std::vector<const char*> menuItems;
  std::vector<UIIcon> menuIcons;
  if (isFengyanTheme) {
    menuItems = {"文件", "微信读书", "番茄", "晋江"};
    menuIcons = {UIIcon::Folder32, UIIcon::Book, UIIcon::Recent, UIIcon::Library};
  } else {
    menuItems = {L(Str::kFileManager), L(Str::kReadingHistory)};
    menuIcons = {UIIcon::Library, UIIcon::Recent};
    if (hasOpdsUrl) {
      menuItems.push_back(L(Str::kOPDSBrowser));
      menuIcons.push_back(UIIcon::Hotspot);
    }
    if (hasjianguoUrl) {
      menuItems.push_back(L(Str::kJianGuoDisk));
      menuIcons.push_back(UIIcon::Transfer);
    }
    if (hasDataCapsuleUrl) {
      menuItems.push_back(L(Str::kDataCapsule));
      menuIcons.push_back(UIIcon::Cog);
    }
    if (hasBookmarkNotes) {
      menuItems.push_back(L(Str::kBookmarkNotes));
      menuIcons.push_back(UIIcon::Book);
    }
    menuItems.push_back(L(Str::kNetworkManage));
    menuIcons.push_back(UIIcon::Wifi);
    menuItems.push_back(L(Str::kApps));
    menuIcons.push_back(UIIcon::Library);
    menuItems.push_back(L(Str::kSystemSettings));
    menuIcons.push_back(UIIcon::Settings);
  }

  // Keep the menu below the cover and above the painted footer. The old
  // height calculation extended the hit/draw grid into the footer.
  Rect menuRect = Rect{0, homeLayout.menuTop, pageWidth, homeLayout.menuHeight};

  // 使用网格布局绘制菜单（一行两个）
  GUI.drawButtonMenu(
      renderer,
      menuRect,
      static_cast<int>(menuItems.size()),
      selectorIndex - recentBooks.size(),
      [&menuItems](int index) { return std::string(menuItems[index]); },
      [&menuIcons](int index) -> UIIcon { return menuIcons[index]; });

  if (isFengyanTheme) {
    GUI.drawButtonHints(renderer, "历史", "应用", "设置", "", true);
  } else {
    const auto labels = mappedInput.mapLabels("", L(Str::kSelect), L(Str::kMoveUp), L(Str::kMoveDown));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  (void)animateHomeEntry;
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  (void)renderer.storeLastShown();

  if (!firstRenderDone) {
    firstRenderDone = true;
    updateRequired.store(true, std::memory_order_release);
  } else if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    loadRecentCovers(metrics.homeCoverWidth, metrics.homeCoverThumbHeight);
    // 封面生成完成，重置渲染状态使下次渲染重新从 SD 卡读取新生成的缩略图
    coverRendered = false;
    coverBufferStored = false;
    freeCoverBuffer();
    updateRequired.store(true, std::memory_order_release);
  }
#endif
}
