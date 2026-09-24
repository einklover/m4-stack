#include "AppListActivity.h"

#include <GfxRenderer.h>

#include "AppInstallActivity.h"
#include "AppRuntimeActivity.h"
#include "BookmarkStore.h"
#include "CrossPointSettings.h"
#include "I18n.h"
#include "MappedInputManager.h"
#include "NativeAppActivity.h"
#include "activities/home/HomeSceneAssetDecoder.h"
#include "apps/M4HomeDock.h"
#include "apps/M4xInstaller.h"
#include "apps/providers/M4Psram.h"
#include "components/icons/book.h"
#include "components/icons/cog.h"
#include "components/icons/folder.h"
#include "components/icons/hotspot.h"
#include "components/icons/library.h"
#include "components/icons/recent.h"
#include "components/icons/settings.h"
#include "components/icons/transfer.h"
#include "components/icons/wifi.h"
#include "components/icons/wifi_transfer.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"
#include "util/M4ListTouchPolicy.h"
// Angle form: the Phase 1 guard contract scans comment/string-masked source,
// so the header anchor must stay visible outside a quoted literal.
#include <util/M4RenderGuard.h>
#include "util/M4ReturnCache.h"
#include "util/M4UiText.h"
#include "util/TouchHitGeometry.h"
#include <Utf8.h>

#include <algorithm>
#include <cstring>

// Phase 1 (INV-R1): process-wide render-submit mutex owned by main.cpp.
extern SemaphoreHandle_t gM4RenderMutex;

namespace {
constexpr unsigned long kAppLongPressMs = 700;
constexpr int kDrawerColumns = 4;
constexpr int kDrawerMinTileHeight = 118;
constexpr int kDrawerGapX = 6;
constexpr int kDrawerGapY = 8;
constexpr int kDrawerPadX = 4;
constexpr int kDrawerIconSlot = 80;
constexpr int kBuiltinIconSize = 32;
constexpr size_t kDrawerLabelMaxChars = 4;

M4ListTouchPolicy::DialogTwoButtonLayout uninstallDialogLayout(const GfxRenderer& renderer) {
  return M4ListTouchPolicy::makeCenteredTwoButtons(renderer.getScreenWidth(), renderer.getScreenHeight() - 190,
                                                   144, 64, 24, 2);
}

TouchHitGeometry::Rect uninstallDataToggleRect(const GfxRenderer& renderer) {
  constexpr int width = 260;
  constexpr int height = 52;
  return {std::max(0, (renderer.getScreenWidth() - width) / 2), renderer.getScreenHeight() - 290, width, height};
}

TouchHitGeometry::Rect dockSlotRect(const GfxRenderer& renderer, int slot) {
  constexpr int width = 190;
  constexpr int height = 64;
  constexpr int gap = 20;
  constexpr int top = 235;
  const int row = slot / 2;
  const int col = slot % 2;
  const int totalWidth = width * 2 + gap;
  const int startX = (renderer.getScreenWidth() - totalWidth) / 2;
  return {startX + col * (width + gap), top + row * (height + 18), width, height};
}

struct DrawerGridLayout {
  int top = 0;
  int bottom = 0;
  int startX = 0;
  int tileWidth = 0;
  int tileHeight = kDrawerMinTileHeight;
  int rows = 1;
  int pageStart = 0;
  int pageItems = kDrawerColumns;
  int itemCount = 0;
  int scrollBarX = 0;
  int scrollBarWidth = 0;
  int scrollTrackHeight = 0;
  bool showScrollBar = false;

  int totalPages() const {
    if (pageItems <= 0) return 1;
    return std::max(1, (itemCount + pageItems - 1) / pageItems);
  }

  int currentPage() const {
    if (pageItems <= 0) return 0;
    return pageStart / pageItems;
  }

  TouchHitGeometry::Rect tileRect(const int index) const {
    if (index < pageStart || index >= pageStart + pageItems || index >= itemCount || tileWidth <= 0) return {};
    const int local = index - pageStart;
    const int col = local % kDrawerColumns;
    const int row = local / kDrawerColumns;
    return {startX + col * (tileWidth + kDrawerGapX), top + row * (tileHeight + kDrawerGapY), tileWidth,
            tileHeight};
  }

  int indexFromPoint(const int x, const int y) const {
    for (int i = pageStart; i < std::min(itemCount, pageStart + pageItems); ++i) {
      if (tileRect(i).contains(x, y)) return i;
    }
    return -1;
  }

  bool scrollBarContains(const int x, const int y) const {
    if (!showScrollBar) return false;
    const int left = scrollBarX - scrollBarWidth - 8;
    return x >= left && x < scrollBarX + 8 && y >= top && y < bottom;
  }

  int pageFromScrollY(const int y) const {
    const int pages = totalPages();
    if (pages <= 1 || scrollTrackHeight <= 0) return 0;
    const int rel = std::min(std::max(y - top, 0), scrollTrackHeight - 1);
    return std::min(pages - 1, (rel * pages) / scrollTrackHeight);
  }
};

DrawerGridLayout makeDrawerGridLayout(const GfxRenderer& renderer, const int selectedIndex, const int itemCount) {
  DrawerGridLayout layout;
  layout.itemCount = std::max(0, itemCount);
  const auto metrics = UITheme::getInstance().getMetrics();
  layout.top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  layout.bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing;
  layout.scrollTrackHeight = std::max(0, layout.bottom - layout.top);

  const int availableHeight = layout.scrollTrackHeight;
  layout.rows = std::max(1, (availableHeight + kDrawerGapY) / (kDrawerMinTileHeight + kDrawerGapY));
  while (layout.rows > 1) {
    const int used = layout.rows * kDrawerMinTileHeight + (layout.rows - 1) * kDrawerGapY;
    if (used <= availableHeight) break;
    --layout.rows;
  }
  const int rowGaps = std::max(0, layout.rows - 1) * kDrawerGapY;
  layout.tileHeight = std::max(kDrawerMinTileHeight,
                               (availableHeight - rowGaps) / std::max(1, layout.rows));
  layout.pageItems = layout.rows * kDrawerColumns;
  if (layout.itemCount > 0) {
    const int safeSelected = std::min(std::max(selectedIndex, 0), layout.itemCount - 1);
    layout.pageStart = (safeSelected / layout.pageItems) * layout.pageItems;
  }
  layout.showScrollBar = layout.itemCount > layout.pageItems;
  layout.scrollBarWidth = metrics.scrollBarWidth;
  layout.scrollBarX = renderer.getScreenWidth() - kDrawerPadX - layout.scrollBarWidth;

  const int rightGutter = layout.showScrollBar ? (layout.scrollBarWidth + 6) : 0;
  const int gridWidth = renderer.getScreenWidth() - 2 * kDrawerPadX - rightGutter;
  layout.tileWidth = std::max(1, (gridWidth - (kDrawerColumns - 1) * kDrawerGapX) / kDrawerColumns);
  layout.startX = kDrawerPadX;
  return layout;
}

void drawDrawerScrollBar(const GfxRenderer& renderer, const DrawerGridLayout& layout) {
  if (!layout.showScrollBar) return;
  const int pages = layout.totalPages();
  const int trackH = layout.scrollTrackHeight;
  if (pages <= 1 || trackH <= 0) return;
  const int thumbH = std::max(layout.scrollBarWidth + 8, (trackH * layout.pageItems) / std::max(1, layout.itemCount));
  const int travel = std::max(0, trackH - thumbH);
  const int thumbY =
      layout.top + (pages > 1 ? (travel * layout.currentPage()) / (pages - 1) : 0);
  renderer.drawLine(layout.scrollBarX, layout.top, layout.scrollBarX, layout.bottom, true);
  renderer.fillRect(layout.scrollBarX - layout.scrollBarWidth, thumbY, layout.scrollBarWidth, thumbH, true);
}

const uint8_t* builtinIconBitmap(const UIIcon icon) {
  switch (icon) {
    case UIIcon::Folder: return FolderIcon;
    case UIIcon::Book: return BookIcon;
    case UIIcon::Recent: return RecentIcon;
    case UIIcon::Settings: return SettingsIcon;
    case UIIcon::Transfer: return TransferIcon;
    case UIIcon::WifiTransfer: return WifiTransferIcon;
    case UIIcon::Library: return LibraryIcon;
    case UIIcon::Wifi: return WifiIcon;
    case UIIcon::Hotspot: return HotspotIcon;
    case UIIcon::Cog: return CogIcon;
    default: return nullptr;
  }
}

void draw1BitIcon(const GfxRenderer& renderer, const uint8_t* icon, const int x, const int y) {
  if (!icon) return;
  for (uint16_t row = 0; row < HomeScene::kHomeAppIconH; ++row) {
    const uint8_t* rowBits = icon + static_cast<size_t>(row) * HomeScene::kHomeAppIconStride;
    for (uint16_t col = 0; col < HomeScene::kHomeAppIconW; ++col) {
      if (rowBits[col >> 3] & static_cast<uint8_t>(0x80 >> (col & 7))) {
        renderer.drawPixel(x + col, y + row, true);
      }
    }
  }
}

}  // namespace

void AppListActivity::drawItemIcon(const DrawerItem& item, const TouchHitGeometry::Rect& tile) const {
  constexpr int iconTopPadding = 8;
  constexpr int labelReserve = 28;
  const int iconSlot = std::max(kDrawerIconSlot, tile.height - labelReserve - iconTopPadding);
  const int iconY = tile.y + iconTopPadding;
  if (item.plugin && !item.pluginIcon.empty()) {
    const int iconX = tile.x + (tile.width - HomeScene::kHomeAppIconW) / 2;
    draw1BitIcon(renderer, item.pluginIcon.data(), iconX,
                 iconY + (iconSlot - HomeScene::kHomeAppIconH) / 2);
    return;
  }

  if (const uint8_t* bitmap = HomeSceneAssetDecoder::builtinSheetIcon(item.id.c_str())) {
    const int iconX = tile.x + (tile.width - HomeScene::kHomeAppIconW) / 2;
    draw1BitIcon(renderer, bitmap, iconX, iconY + (iconSlot - HomeScene::kHomeAppIconH) / 2);
    return;
  }

  const uint8_t* bitmap = builtinIconBitmap(item.icon);
  if (!bitmap) return;
  const int iconX = tile.x + (tile.width - kBuiltinIconSize) / 2;
  renderer.drawIcon(bitmap, iconX, iconY + (iconSlot - kBuiltinIconSize) / 2, kBuiltinIconSize,
                    kBuiltinIconSize);
}

void AppListActivity::taskTrampoline(void* param) {
  static_cast<AppListActivity*>(param)->displayTaskLoop();
}

void AppListActivity::displayTaskLoop() {
  while (true) {
    if (exitDisplayTask_.load(std::memory_order_acquire)) {
      // Cooperative shutdown: exit only while holding no locks, so the
      // process-wide guard is never left owned by a deleted task.
      displayTaskHandle_ = nullptr;
      displayTaskExited_.store(true, std::memory_order_release);
      Serial.printf("[WRPERF] stage=applist-display-task-exit stack_hwm=%u\n",
                    static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
      M4Psram::deleteTask(nullptr);
      for (;;) vTaskDelay(portMAX_DELAY);
    }
    if (childScreenOwned_.load(std::memory_order_acquire)) {
      vTaskDelay(10 / portTICK_PERIOD_MS);
      continue;
    }
    if (updateRequired_) {
      AppListFrameSnapshot snapshot;
      bool shouldSubmit = false;
      // Phase 1: snapshot under the local mutex, then release it before taking
      // the global guard, so global is never acquired while holding local.
      if (xSemaphoreTake(renderingMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
        // Child installation happens after releasing this mutex. Recheck
        // after taking it so a stale pre-lock observation cannot paint the drawer
        // over the child's first frame.
        if (updateRequired_ && !childScreenOwned_.load(std::memory_order_acquire)) {
          snapshot.selectedIndex = selectedIndex_;
          snapshot.mode = mode_;
          snapshot.uninstallClearData = uninstallClearData_;
          snapshot.items = items_;
          snapshot.apps = apps_;
          snapshot_ = snapshot;
          updateRequired_ = false;
          shouldSubmit = true;
        }
        xSemaphoreGive(renderingMutex_);
      }
      // A child installed after the snapshot owns the screen now: drop the
      // stale parent frame instead of painting over the plugin. (Child close
      // re-arms updateRequired_ via reload, so nothing is lost.)
      if (shouldSubmit) {
        M4RenderGuard renderGuard(gM4RenderMutex);
        if (renderGuard.owns() && !childScreenOwned_.load(std::memory_order_acquire)) {
          render();
          showedDrawer_ = true;
        } else {
          // Global contended: never submit without the guard; re-arm instead.
          if (xSemaphoreTake(renderingMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
            updateRequired_ = true;
            xSemaphoreGive(renderingMutex_);
          }
        }
      }
    }
    if (verifyDrawerCache_ && showedDrawer_ && mode_ == 0 &&
        !childScreenOwned_.load(std::memory_order_acquire) &&
        !exitDisplayTask_.load(std::memory_order_acquire)) {
      reloadPreserveDialog_ = true;
      reload();
      if (reloadChanged_) {
        if (xSemaphoreTake(renderingMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
          updateRequired_ = true;
          xSemaphoreGive(renderingMutex_);
        }
      } else if (mode_ == 0) {
        verifyDrawerCache_ = false;
      }
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

namespace {

bool sameInstalledApp(const M4xInstalledApp& a, const M4xInstalledApp& b) {
  return a.id == b.id && a.name == b.name && a.version == b.version && a.versionCode == b.versionCode &&
         a.path == b.path && a.runtime == b.runtime && a.entry == b.entry && a.provider == b.provider &&
         a.icon == b.icon && a.installedAt == b.installedAt;
}

}  // namespace

bool AppListActivity::sameDrawer(const std::vector<M4xInstalledApp>& appsA, const std::vector<DrawerItem>& itemsA,
                                 const std::vector<M4xInstalledApp>& appsB, const std::vector<DrawerItem>& itemsB) {
  if (appsA.size() != appsB.size() || itemsA.size() != itemsB.size()) return false;
  for (size_t i = 0; i < appsA.size(); ++i) {
    if (!sameInstalledApp(appsA[i], appsB[i])) return false;
  }
  for (size_t i = 0; i < itemsA.size(); ++i) {
    const DrawerItem& a = itemsA[i];
    const DrawerItem& b = itemsB[i];
    if (a.plugin != b.plugin || a.builtin != b.builtin || a.appIndex != b.appIndex || a.id != b.id ||
        a.label != b.label || a.icon != b.icon || a.pluginIcon.size() != b.pluginIcon.size()) {
      return false;
    }
    if (!a.pluginIcon.empty() && std::memcmp(a.pluginIcon.data(), b.pluginIcon.data(), a.pluginIcon.size()) != 0) {
      return false;
    }
  }
  return true;
}

bool AppListActivity::applyCachedDrawer() {
  std::vector<M4xInstalledApp> apps;
  std::vector<M4ReturnCache::DrawerItem> cached;
  if (!M4ReturnCache::restoreDrawer(apps, cached)) return false;
  std::vector<DrawerItem> items;
  items.reserve(cached.size());
  for (const auto& item : cached) {
    DrawerItem out;
    out.plugin = item.plugin;
    out.builtin = static_cast<BuiltinAction>(item.builtin);
    out.appIndex = item.appIndex;
    out.id = item.id;
    out.label = item.label;
    out.icon = static_cast<UIIcon>(item.icon);
    out.pluginIcon = item.pluginIcon;
    items.push_back(std::move(out));
  }
  if (renderingMutex_) xSemaphoreTake(renderingMutex_, portMAX_DELAY);
  apps_ = std::move(apps);
  items_ = std::move(items);
  if (selectedIndex_ >= static_cast<int>(items_.size())) {
    selectedIndex_ = std::max(0, static_cast<int>(items_.size()) - 1);
  }
  mode_ = 0;
  M4FooterTouchPolicy::setMask(touchFooterButtonsMask());
  if (renderingMutex_) xSemaphoreGive(renderingMutex_);
  return true;
}

void AppListActivity::reload() {
  reloadChanged_ = false;
  const bool preserveDialog = reloadPreserveDialog_;
  reloadPreserveDialog_ = false;
  if (reloadLock_) xSemaphoreTake(reloadLock_, portMAX_DELAY);
  struct LockRelease {
    SemaphoreHandle_t sem;
    ~LockRelease() {
      if (sem) xSemaphoreGive(sem);
    }
  } release{reloadLock_};

  if (exitDisplayTask_.load(std::memory_order_acquire)) return;
  M4xInstaller::ensureLayout();
  const auto apps = M4xRegistry::load();
  if (exitDisplayTask_.load(std::memory_order_acquire)) return;

  std::vector<DrawerItem> items;
  items.reserve(8 + apps.size());
  const auto addBuiltin = [&items](const BuiltinAction action, const char* id, const char* label,
                                   const UIIcon icon) {
    DrawerItem item;
    item.builtin = action;
    item.id = id ? id : "";
    item.label = label ? label : "";
    item.icon = icon;
    items.push_back(std::move(item));
  };

  // Keep the non-Fengyan home destinations available from the drawer. Optional
  // entries use the same configured-state checks as the legacy home menu.
  addBuiltin(BuiltinAction::FileManager, "builtin.files", L(Str::kFileManager), UIIcon::Folder);
  addBuiltin(BuiltinAction::FileTransfer, "builtin.transfer", L(Str::kWifiTransfer), UIIcon::WifiTransfer);
  addBuiltin(BuiltinAction::RecentBooks, "builtin.history", L(Str::kReadingHistory), UIIcon::Recent);
  if (std::strlen(SETTINGS.opdsServerUrl) > 0) {
    addBuiltin(BuiltinAction::Opds, "builtin.opds", L(Str::kOPDSBrowser), UIIcon::Hotspot);
  }
  if (std::strlen(SETTINGS.jgUsername) > 0) {
    addBuiltin(BuiltinAction::JianGuo, "builtin.jianguo", L(Str::kJianGuoDisk), UIIcon::Transfer);
  }
  if (std::strlen(SETTINGS.dcUsername) > 0) {
    addBuiltin(BuiltinAction::DataCapsule, "builtin.datacapsule", L(Str::kDataCapsule), UIIcon::Cog);
  }
  if (BookmarkStore::hasAnyBookmarks()) {
    addBuiltin(BuiltinAction::BookmarkNotes, "builtin.bookmarks", L(Str::kBookmarkNotes), UIIcon::Book);
  }
  addBuiltin(BuiltinAction::Network, "builtin.network", L(Str::kNetworkManage), UIIcon::Wifi);
  addBuiltin(BuiltinAction::Settings, "builtin.settings", L(Str::kSystemSettings), UIIcon::Settings);

  for (size_t i = 0; i < apps.size(); ++i) {
    const auto& app = apps[i];
    DrawerItem item;
    item.plugin = true;
    item.appIndex = static_cast<int>(i);
    item.id = app.id;
    item.label = app.name.empty() ? app.id : app.name;
    item.icon = UIIcon::Library;

    const bool cachedIcon = M4ReturnCache::copyDrawerIcon(app.id, app.versionCode, app.path, app.icon,
                                                          app.installedAt, item.pluginIcon) &&
                            item.pluginIcon.size() == HomeScene::kHomeAppIconBytes;
    if (!cachedIcon) {
      item.pluginIcon.clear();
      const std::string iconPath = HomeSceneAssetDecoder::resolveAppIconPath(app.path, app.icon);
      if (!iconPath.empty()) {
        item.pluginIcon.resize(HomeScene::kHomeAppIconBytes);
        if (!HomeSceneAssetDecoder::decodeBmpFileTo1Bit(iconPath.c_str(), item.pluginIcon.data(),
                                                         HomeScene::kHomeAppIconW, HomeScene::kHomeAppIconH,
                                                         HomeScene::kHomeAppIconStride)) {
          item.pluginIcon.clear();
        }
      }
    }
    items.push_back(std::move(item));
  }

  if (renderingMutex_) xSemaphoreTake(renderingMutex_, portMAX_DELAY);
  if (preserveDialog && mode_ != 0) {
    verifyDrawerCache_ = true;
    if (renderingMutex_) xSemaphoreGive(renderingMutex_);
    return;
  }
  const bool changed = !sameDrawer(apps_, items_, apps, items);
  if (!changed) {
    if (renderingMutex_) xSemaphoreGive(renderingMutex_);
    return;
  }
  std::vector<M4ReturnCache::DrawerItem> cachedItems;
  cachedItems.reserve(items.size());
  for (const auto& item : items) {
    M4ReturnCache::DrawerItem cached;
    cached.plugin = item.plugin;
    cached.builtin = static_cast<uint8_t>(item.builtin);
    cached.appIndex = item.appIndex;
    cached.id = item.id;
    cached.label = item.label;
    cached.icon = static_cast<int>(item.icon);
    cached.pluginIcon = item.pluginIcon;
    cachedItems.push_back(std::move(cached));
  }
  apps_ = apps;
  items_ = std::move(items);
  if (selectedIndex_ >= static_cast<int>(items_.size())) {
    selectedIndex_ = std::max(0, static_cast<int>(items_.size()) - 1);
  }
  mode_ = 0;
  M4FooterTouchPolicy::setMask(touchFooterButtonsMask());
  reloadChanged_ = true;
  if (renderingMutex_) xSemaphoreGive(renderingMutex_);
  M4ReturnCache::rememberDrawer(apps, cachedItems);
}

void AppListActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  renderingMutex_ = xSemaphoreCreateMutex();
  reloadLock_ = xSemaphoreCreateMutex();
  exitDisplayTask_.store(false, std::memory_order_release);
  displayTaskExited_.store(false, std::memory_order_release);
  childScreenOwned_.store(false, std::memory_order_release);
  showedDrawer_ = false;
  if (applyCachedDrawer()) {
    verifyDrawerCache_ = true;
  } else {
    reload();
    verifyDrawerCache_ = false;
  }
  updateRequired_ = true;
  if (M4Psram::createTask(&AppListActivity::taskTrampoline, "AppList", 8192, this, 1,
                          &displayTaskHandle_) != pdPASS) {
    displayTaskHandle_ = nullptr;
    displayTaskExited_.store(true, std::memory_order_release);
    Serial.printf("[%lu] [AppList] failed to create display task\n", millis());
  }
}

void AppListActivity::onExit() {
  childScreenOwned_.store(true, std::memory_order_release);
  ActivityWithSubactivity::onExit();
  // Cooperative display-task shutdown (same pattern as MyLibrary): the task
  // may own the process-wide submit guard mid-render; deleting it then would
  // stick the mutex for all activities. Ask it to self-terminate (it exits
  // holding no locks) and join boundedly; force-delete only past the deadline.
  exitDisplayTask_.store(true, std::memory_order_release);
  for (int i = 0; i < 300; ++i) {
    if (displayTaskExited_.load(std::memory_order_acquire)) break;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (renderingMutex_) {
    const bool exitLocked = (xSemaphoreTake(renderingMutex_, pdMS_TO_TICKS(100)) == pdTRUE);
    if (displayTaskHandle_) {
      // Deadline overrun: last resort (may strand an in-flight submit).
      M4Psram::deleteTask(displayTaskHandle_);
      displayTaskHandle_ = nullptr;
    }
    if (exitLocked) xSemaphoreGive(renderingMutex_);
    vSemaphoreDelete(renderingMutex_);
    renderingMutex_ = nullptr;
  } else if (displayTaskHandle_) {
    vTaskDelete(displayTaskHandle_);
    displayTaskHandle_ = nullptr;
  }
  if (reloadLock_) {
    vSemaphoreDelete(reloadLock_);
    reloadLock_ = nullptr;
  }
}

bool AppListActivity::selectedIsPlugin() const {
  return selectedIndex_ >= 0 && selectedIndex_ < static_cast<int>(items_.size()) &&
         items_[static_cast<size_t>(selectedIndex_)].plugin;
}

void AppListActivity::selectIndex(const int index) {
  if (items_.empty()) return;
  // Short lock scope over the render-consumed writes. moveSelection() delegates
  // here, so it takes no lock itself (the mutex is non-recursive: no double-take).
  if (renderingMutex_ && xSemaphoreTake(renderingMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
    selectedIndex_ = std::min(std::max(index, 0), static_cast<int>(items_.size()) - 1);
    M4FooterTouchPolicy::setMask(touchFooterButtonsMask());
    updateRequired_ = true;
    xSemaphoreGive(renderingMutex_);
  }
}

void AppListActivity::moveSelection(const int delta) {
  if (items_.empty() || delta == 0) return;
  const int count = static_cast<int>(items_.size());
  int next = selectedIndex_ + delta;
  next %= count;
  if (next < 0) next += count;
  selectIndex(next);
}

void AppListActivity::pageBy(const int pages) {
  if (items_.empty() || pages == 0) return;
  const int count = static_cast<int>(items_.size());
  const auto layout = makeDrawerGridLayout(renderer, selectedIndex_, count);
  selectIndex(M4ListTouchPolicy::applyPage(selectedIndex_, count, layout.pageItems, pages > 0));
}

void AppListActivity::activateBuiltin(const BuiltinAction action) {
  switch (action) {
    case BuiltinAction::FileManager:
      if (callbacks_.onFileManagerOpen) callbacks_.onFileManagerOpen();
      return;
    case BuiltinAction::FileTransfer:
      if (callbacks_.onFileTransferOpen) callbacks_.onFileTransferOpen();
      return;
    case BuiltinAction::RecentBooks:
      if (callbacks_.onRecentBooksOpen) callbacks_.onRecentBooksOpen();
      return;
    case BuiltinAction::Opds:
      if (callbacks_.onOpdsOpen) callbacks_.onOpdsOpen();
      return;
    case BuiltinAction::JianGuo:
      if (callbacks_.onJianGuoOpen) callbacks_.onJianGuoOpen();
      return;
    case BuiltinAction::DataCapsule:
      if (callbacks_.onDataCapsuleOpen) callbacks_.onDataCapsuleOpen();
      return;
    case BuiltinAction::BookmarkNotes:
      if (callbacks_.onBookmarkNotesOpen) callbacks_.onBookmarkNotesOpen();
      return;
    case BuiltinAction::Network:
      if (callbacks_.onNetworkOpen) callbacks_.onNetworkOpen();
      return;
    case BuiltinAction::Settings:
      if (callbacks_.onSettingsOpen) callbacks_.onSettingsOpen();
      return;
  }
}

void AppListActivity::openSelected() {
  // Copy the needed item/app data to locals under the local mutex, release it,
  // then install the child: never hold local across enterNewActivity, so no
  // local->global nesting is possible on this path.
  DrawerItem selectedItem;
  M4xInstalledApp selectedApp;
  bool haveSelection = false;
  bool haveApp = false;
  if (renderingMutex_ && xSemaphoreTake(renderingMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (selectedIndex_ >= 0 && selectedIndex_ < static_cast<int>(items_.size())) {
      selectedItem = items_[static_cast<size_t>(selectedIndex_)];
      haveSelection = true;
      if (selectedItem.plugin && selectedItem.appIndex >= 0 &&
          selectedItem.appIndex < static_cast<int>(apps_.size())) {
        selectedApp = apps_[static_cast<size_t>(selectedItem.appIndex)];
        haveApp = true;
      }
    }
    xSemaphoreGive(renderingMutex_);
  }
  if (!haveSelection) return;
  if (!selectedItem.plugin) {
    activateBuiltin(selectedItem.builtin);
    return;
  }
  if (!haveApp) return;
  childScreenOwned_.store(true, std::memory_order_release);
  if (renderingMutex_ && xSemaphoreTake(renderingMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
    updateRequired_ = false;
    xSemaphoreGive(renderingMutex_);
  }
  auto onClosed = [this]() { requestExitSubActivity(); };
  if (selectedApp.runtime == M4xRuntimeKind::Native) {
    enterNewActivity(new NativeAppActivity(renderer, mappedInput, selectedApp, onClosed));
  } else {
    enterNewActivity(new AppRuntimeActivity(renderer, mappedInput, selectedApp, onClosed));
  }
}

void AppListActivity::openInstall() {
  // Handoff barrier: order child installation after any in-flight frame
  // snapshot, then release before enter so local is never held across it.
  childScreenOwned_.store(true, std::memory_order_release);
  if (renderingMutex_ && xSemaphoreTake(renderingMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
    updateRequired_ = false;
    xSemaphoreGive(renderingMutex_);
  }
  enterNewActivity(new AppInstallActivity(renderer, mappedInput, "", [this]() {
    requestExitSubActivity();
  }));
}

void AppListActivity::openContextMenu() {
  if (!selectedIsPlugin()) return;
  if (renderingMutex_ && xSemaphoreTake(renderingMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
    mode_ = 2;
    M4FooterTouchPolicy::setMask(touchFooterButtonsMask());
    updateRequired_ = true;
    xSemaphoreGive(renderingMutex_);
  }
}

void AppListActivity::pinSelected(const int slot) {
  if (!selectedIsPlugin() || slot < 0 || slot >= M4HomeDock::kSlotCount) return;
  const auto& item = items_[static_cast<size_t>(selectedIndex_)];
  if (item.id.empty()) return;
  if (!M4HomeDock::setSlot(slot, item.id)) {
    Serial.printf("[M4x] failed to pin %s to home slot %d\n", item.id.c_str(), slot + 1);
    return;
  }
  Serial.printf("[M4x] pinned %s to home slot %d\n", item.id.c_str(), slot + 1);
  if (renderingMutex_ && xSemaphoreTake(renderingMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
    mode_ = 0;
    M4FooterTouchPolicy::setMask(touchFooterButtonsMask());
    updateRequired_ = true;
    xSemaphoreGive(renderingMutex_);
  }
}

void AppListActivity::uninstallSelected() {
  if (!selectedIsPlugin()) return;
  const auto& item = items_[static_cast<size_t>(selectedIndex_)];
  if (item.appIndex < 0 || item.appIndex >= static_cast<int>(apps_.size())) return;
  const std::string appId = apps_[static_cast<size_t>(item.appIndex)].id;
  std::string err;
  if (!M4xInstaller::uninstall(appId, uninstallClearData_, err)) {
    Serial.printf("[M4x] uninstall failed: %s\n", err.c_str());
  } else {
    M4HomeDock::clear(appId);
  }
  reload();
  if (renderingMutex_ && xSemaphoreTake(renderingMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
    updateRequired_ = true;
    xSemaphoreGive(renderingMutex_);
  }
}

void AppListActivity::loop() {
  if (subActivity) {
    if (pumpSubActivityFrame()) {
      const bool warmed = applyCachedDrawer();
      childScreenOwned_.store(false, std::memory_order_release);
      if (warmed) {
        verifyDrawerCache_ = true;
        if (renderingMutex_ && xSemaphoreTake(renderingMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
          updateRequired_ = true;
          xSemaphoreGive(renderingMutex_);
        }
      } else {
        reload();
        if (renderingMutex_ && xSemaphoreTake(renderingMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
          updateRequired_ = true;
          xSemaphoreGive(renderingMutex_);
        }
      }
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) {
    if (mode_ == 1 || mode_ == 2) {
      if (renderingMutex_ && xSemaphoreTake(renderingMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
        mode_ = 0;
        M4FooterTouchPolicy::setMask(touchFooterButtonsMask());
        updateRequired_ = true;
        xSemaphoreGive(renderingMutex_);
      }
    } else {
      onGoBack();
    }
    return;
  }

  const int count = static_cast<int>(items_.size());
  if (mode_ == 2) {
    int tx = 0, ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty)) {
      for (int slot = 0; slot < M4HomeDock::kSlotCount; ++slot) {
        if (dockSlotRect(renderer, slot).contains(tx, ty)) {
          pinSelected(slot);
          return;
        }
      }
      const auto dialog = uninstallDialogLayout(renderer);
      int hit = -1;
      if (M4ListTouchPolicy::dialogButtonFromPoint(dialog, tx, ty, hit)) {
        if (hit == 0) {
          if (renderingMutex_ && xSemaphoreTake(renderingMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
            mode_ = 0;
            M4FooterTouchPolicy::setMask(touchFooterButtonsMask());
            updateRequired_ = true;
            xSemaphoreGive(renderingMutex_);
          }
        } else {
          mode_ = 1;
          M4FooterTouchPolicy::setMask(touchFooterButtonsMask());
          updateRequired_ = true;
        }
      }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      pinSelected(0);
    }
    return;
  }

  if (mode_ == 1) {
    int tx = 0, ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty)) {
      if (ty >= renderer.getScreenHeight() - UITheme::getInstance().getMetrics().buttonHintsHeight) {
        if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) uninstallSelected();
        return;
      }
      const auto dialog = uninstallDialogLayout(renderer);
      int hit = -1;
      if (M4ListTouchPolicy::dialogButtonFromPoint(dialog, tx, ty, hit)) {
        if (hit == 0) {
          if (renderingMutex_ && xSemaphoreTake(renderingMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
            mode_ = 0;
            M4FooterTouchPolicy::setMask(touchFooterButtonsMask());
            updateRequired_ = true;
            xSemaphoreGive(renderingMutex_);
          }
        } else {
          uninstallSelected();
        }
      } else if (uninstallDataToggleRect(renderer).contains(tx, ty)) {
        if (renderingMutex_ && xSemaphoreTake(renderingMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
          uninstallClearData_ = !uninstallClearData_;
          updateRequired_ = true;
          xSemaphoreGive(renderingMutex_);
        }
      }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      uninstallSelected();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      if (renderingMutex_ && xSemaphoreTake(renderingMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
        uninstallClearData_ = !uninstallClearData_;
        updateRequired_ = true;
        xSemaphoreGive(renderingMutex_);
      }
    }
    return;
  }

  if (mappedInput.hasTouch()) {
    int tx = 0, ty = 0;
    const bool tapped = mappedInput.wasScreenTapped(tx, ty);
    const auto layout = makeDrawerGridLayout(renderer, selectedIndex_, count);
    if (tapped) {
      if (ty >= layout.bottom) {
        if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
          onGoBack();
        } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
          openSelected();
        } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
          openInstall();
        } else if (mappedInput.wasReleased(MappedInputManager::Button::Left) && selectedIsPlugin()) {
          if (renderingMutex_ && xSemaphoreTake(renderingMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
            mode_ = 1;
            M4FooterTouchPolicy::setMask(touchFooterButtonsMask());
            updateRequired_ = true;
            xSemaphoreGive(renderingMutex_);
          }
        }
        return;
      }

      if (layout.scrollBarContains(tx, ty)) {
        const int page = layout.pageFromScrollY(ty);
        selectIndex(page * layout.pageItems);
        return;
      }

      const int hit = layout.indexFromPoint(tx, ty);
      if (hit >= 0) {
        selectIndex(hit);
        if (selectedIsPlugin() && mappedInput.lastScreenTouchHeldMs() >= kAppLongPressMs) {
          openContextMenu();
        } else {
          openSelected();
        }
        return;
      }
    }

    const auto sw = mappedInput.wasSwipe();
    if (sw != MappedInputManager::SwipeDir::None && count > 0) {
      switch (sw) {
        case MappedInputManager::SwipeDir::Left: moveSelection(-1); break;
        case MappedInputManager::SwipeDir::Right: moveSelection(1); break;
        case MappedInputManager::SwipeDir::Up: pageBy(1); break;
        case MappedInputManager::SwipeDir::Down: pageBy(-1); break;
        case MappedInputManager::SwipeDir::None: break;
      }
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (count > 0) openSelected();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left) && selectedIsPlugin()) {
    if (renderingMutex_ && xSemaphoreTake(renderingMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
      mode_ = 1;
      M4FooterTouchPolicy::setMask(touchFooterButtonsMask());
      updateRequired_ = true;
      xSemaphoreGive(renderingMutex_);
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    openInstall();
    return;
  }

  if (count > 0) {
    ButtonNavigator navigator;
    navigator.onRelease({MappedInputManager::Button::Up}, [this] { moveSelection(-kDrawerColumns); });
    navigator.onRelease({MappedInputManager::Button::Down}, [this] { moveSelection(kDrawerColumns); });
  }
}

void AppListActivity::render() const {
  // Submit-path input staged by displayTaskLoop(): it deep-copies the dirty
  // frame under the local mutex, releases it, then calls render() under the
  // global guard. Reading through this const reference keeps the submitter on
  // one generation even though reload() replaces items_/apps_ wholesale;
  // members stay the backing store for loop() hit-testing on the main thread.
  const AppListFrameSnapshot& frame = snapshot_;
  const bool framePluginSelected =
      frame.selectedIndex >= 0 && frame.selectedIndex < static_cast<int>(frame.items.size()) &&
      frame.items[static_cast<size_t>(frame.selectedIndex)].plugin;
  renderer.clearScreen();
  const auto metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, L(Str::kApps));

  if (frame.mode == 2 && framePluginSelected) {
    const auto& item = frame.items[static_cast<size_t>(frame.selectedIndex)];
    const auto& app = frame.apps[static_cast<size_t>(item.appIndex)];
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, 100, "扩展应用", true, EpdFontFamily::BOLD);
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 150, app.name.c_str());
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 190, "固定到主页的位置");
    for (int slot = 0; slot < M4HomeDock::kSlotCount; ++slot) {
      const auto r = dockSlotRect(renderer, slot);
      renderer.fillRoundedRect(r.x, r.y, r.width, r.height, 12, Color::LightGray);
      char label[24];
      std::snprintf(label, sizeof(label), "主页位置 %d", slot + 1);
      M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, r.x, r.y, r.width, r.height, label, true,
                                  EpdFontFamily::BOLD, 8);
    }
    const auto dialog = uninstallDialogLayout(renderer);
    const auto drawContextButton = [&](const int index, const char* label) {
      const auto r = dialog.buttonRect(index);
      renderer.fillRoundedRect(r.x, r.y, r.width, r.height, 12, index == 1 ? Color::Black : Color::LightGray);
      M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, r.x, r.y, r.width, r.height, label, index == 0,
                                  EpdFontFamily::BOLD, 8);
    };
    drawContextButton(0, L(Str::kCancel));
    drawContextButton(1, L(Str::kUninstallApp));
  } else if (frame.mode == 1 && framePluginSelected) {
    const auto& item = frame.items[static_cast<size_t>(frame.selectedIndex)];
    const auto& app = frame.apps[static_cast<size_t>(item.appIndex)];
    const auto dialog = uninstallDialogLayout(renderer);
    const auto toggle = uninstallDataToggleRect(renderer);
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, 100, L(Str::kUninstallApp), true, EpdFontFamily::BOLD);
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 155, app.name.c_str());
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 195, "确认移除这个扩展应用？");
    renderer.fillRoundedRect(toggle.x, toggle.y, toggle.width, toggle.height, 10, Color::LightGray);
    M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, toggle.x, toggle.y, toggle.width, toggle.height,
                                frame.uninstallClearData ? "同时清除数据：是" : "同时清除数据：否", true,
                                EpdFontFamily::REGULAR, 8);
    const auto drawDialogButton = [&](const int index, const char* label) {
      const auto r = dialog.buttonRect(index);
      renderer.fillRoundedRect(r.x, r.y, r.width, r.height, 12, index == 1 ? Color::Black : Color::LightGray);
      M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, r.x, r.y, r.width, r.height, label, index == 0,
                                  EpdFontFamily::BOLD, 8);
    };
    drawDialogButton(0, L(Str::kCancel));
    drawDialogButton(1, L(Str::kUninstallApp));
  } else {
    const auto layout =
        makeDrawerGridLayout(renderer, frame.selectedIndex, static_cast<int>(frame.items.size()));
    for (int i = layout.pageStart; i < std::min(layout.itemCount, layout.pageStart + layout.pageItems); ++i) {
      const auto tile = layout.tileRect(i);
      const bool selected = i == frame.selectedIndex;
      if (selected) renderer.fillRoundedRect(tile.x, tile.y, tile.width, tile.height, 12, Color::LightGray);
      const auto& item = frame.items[static_cast<size_t>(i)];
      drawItemIcon(item, tile);

      // Character-count ellipsis, not pixel-width: four CJK glyphs must stay
      // intact (「文件管理」). Pixel truncate at tile.width-8 became 「文件管…」.
      const std::string label = utf8EllipsizeChars(item.label.c_str(), kDrawerLabelMaxChars);
      const int labelWidth = M4UiText::textWidth(renderer, UI_12_FONT_ID, label.c_str());
      const int labelY = tile.y + tile.height - 24;
      M4UiText::draw(renderer, UI_12_FONT_ID, tile.x + std::max(0, (tile.width - labelWidth) / 2), labelY,
                     label.c_str(), true);
    }
    drawDrawerScrollBar(renderer, layout);
  }

  const bool pluginSelected = framePluginSelected;
  const auto labels = (frame.mode == 1 || frame.mode == 2)
                          ? mappedInput.mapLabels(L(Str::kBackShort), L(Str::kConfirm), "", "")
                          : mappedInput.mapLabels(L(Str::kBackShort), L(Str::kOpen),
                                                  pluginSelected ? L(Str::kUninstallApp) : "",
                                                  L(Str::kInstallApp));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
