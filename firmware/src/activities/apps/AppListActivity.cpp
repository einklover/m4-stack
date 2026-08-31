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
#include "apps/M4xInstaller.h"
#include "components/icons/book.h"
#include "components/icons/cog.h"
#include "components/icons/folder.h"
#include "components/icons/hotspot.h"
#include "components/icons/library.h"
#include "components/icons/recent.h"
#include "components/icons/settings.h"
#include "components/icons/transfer.h"
#include "components/icons/wifi.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4ListTouchPolicy.h"
#include "util/M4UiText.h"
#include "util/TouchHitGeometry.h"
#include <Utf8.h>

#include <algorithm>
#include <cstring>

namespace {
constexpr unsigned long kAppLongPressMs = 700;
constexpr int kDrawerColumns = 3;
constexpr int kDrawerTileHeight = 120;
constexpr int kDrawerGapX = 8;
constexpr int kDrawerGapY = 8;
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

struct DrawerGridLayout {
  int top = 0;
  int bottom = 0;
  int startX = 0;
  int tileWidth = 0;
  int tileHeight = kDrawerTileHeight;
  int rows = 1;
  int pageStart = 0;
  int pageItems = kDrawerColumns;
  int itemCount = 0;

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
};

DrawerGridLayout makeDrawerGridLayout(const GfxRenderer& renderer, const int selectedIndex, const int itemCount) {
  DrawerGridLayout layout;
  layout.itemCount = std::max(0, itemCount);
  const auto metrics = UITheme::getInstance().getMetrics();
  layout.top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  layout.bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing;

  const int availableHeight = std::max(0, layout.bottom - layout.top);
  layout.rows = std::max(1, (availableHeight + kDrawerGapY) / (kDrawerTileHeight + kDrawerGapY));
  layout.pageItems = layout.rows * kDrawerColumns;
  if (layout.itemCount > 0) {
    const int safeSelected = std::min(std::max(selectedIndex, 0), layout.itemCount - 1);
    layout.pageStart = (safeSelected / layout.pageItems) * layout.pageItems;
  }

  const int gridWidth = renderer.getScreenWidth() - 2 * metrics.contentSidePadding;
  layout.tileWidth = std::max(1, (gridWidth - (kDrawerColumns - 1) * kDrawerGapX) / kDrawerColumns);
  const int actualGridWidth = kDrawerColumns * layout.tileWidth + (kDrawerColumns - 1) * kDrawerGapX;
  layout.startX = std::max(0, (renderer.getScreenWidth() - actualGridWidth) / 2);
  return layout;
}

const uint8_t* builtinIconBitmap(const UIIcon icon) {
  switch (icon) {
    case UIIcon::Folder: return FolderIcon;
    case UIIcon::Book: return BookIcon;
    case UIIcon::Recent: return RecentIcon;
    case UIIcon::Settings: return SettingsIcon;
    case UIIcon::Transfer: return TransferIcon;
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
  const int iconY = tile.y + iconTopPadding;
  if (item.plugin && !item.pluginIcon.empty()) {
    const int iconX = tile.x + (tile.width - HomeScene::kHomeAppIconW) / 2;
    draw1BitIcon(renderer, item.pluginIcon.data(), iconX,
                 iconY + (kDrawerIconSlot - HomeScene::kHomeAppIconH) / 2);
    return;
  }

  if (const uint8_t* bitmap = HomeSceneAssetDecoder::builtinSheetIcon(item.id.c_str())) {
    const int iconX = tile.x + (tile.width - HomeScene::kHomeAppIconW) / 2;
    draw1BitIcon(renderer, bitmap, iconX, iconY + (kDrawerIconSlot - HomeScene::kHomeAppIconH) / 2);
    return;
  }

  const uint8_t* bitmap = builtinIconBitmap(item.icon);
  if (!bitmap) return;
  const int iconX = tile.x + (tile.width - kBuiltinIconSize) / 2;
  renderer.drawIcon(bitmap, iconX, iconY + (kDrawerIconSlot - kBuiltinIconSize) / 2, kBuiltinIconSize,
                    kBuiltinIconSize);
}

void AppListActivity::taskTrampoline(void* param) {
  static_cast<AppListActivity*>(param)->displayTaskLoop();
}

void AppListActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired_) {
      xSemaphoreTake(renderingMutex_, portMAX_DELAY);
      // openSelected() installs the child while holding this mutex. Recheck
      // after taking it so a stale pre-lock observation cannot paint the drawer
      // over the child's first frame.
      if (updateRequired_ && !subActivity) {
        updateRequired_ = false;
        render();
      }
      xSemaphoreGive(renderingMutex_);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void AppListActivity::reload() {
  M4xInstaller::ensureLayout();
  const auto apps = M4xRegistry::load();

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
  addBuiltin(BuiltinAction::FileTransfer, "builtin.files", L(Str::kFileManager), UIIcon::Folder);
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

    const std::string iconPath = HomeSceneAssetDecoder::resolveAppIconPath(app.path, app.icon);
    if (!iconPath.empty()) {
      item.pluginIcon.resize(HomeScene::kHomeAppIconBytes);
      if (!HomeSceneAssetDecoder::decodeBmpFileTo1Bit(iconPath.c_str(), item.pluginIcon.data(),
                                                       HomeScene::kHomeAppIconW, HomeScene::kHomeAppIconH,
                                                       HomeScene::kHomeAppIconStride)) {
        item.pluginIcon.clear();
      }
    }
    items.push_back(std::move(item));
  }

  if (renderingMutex_) xSemaphoreTake(renderingMutex_, portMAX_DELAY);
  apps_ = apps;
  items_ = std::move(items);
  if (selectedIndex_ >= static_cast<int>(items_.size())) {
    selectedIndex_ = std::max(0, static_cast<int>(items_.size()) - 1);
  }
  mode_ = 0;
  M4FooterTouchPolicy::setMask(touchFooterButtonsMask());
  if (renderingMutex_) xSemaphoreGive(renderingMutex_);
}

void AppListActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  renderingMutex_ = xSemaphoreCreateMutex();
  reload();
  updateRequired_ = true;
  xTaskCreate(&AppListActivity::taskTrampoline, "AppList", 4096, this, 1, &displayTaskHandle_);
}

void AppListActivity::onExit() {
  ActivityWithSubactivity::onExit();
  if (renderingMutex_) xSemaphoreTake(renderingMutex_, portMAX_DELAY);
  if (displayTaskHandle_) {
    vTaskDelete(displayTaskHandle_);
    displayTaskHandle_ = nullptr;
  }
  if (renderingMutex_) {
    xSemaphoreGive(renderingMutex_);
    vSemaphoreDelete(renderingMutex_);
    renderingMutex_ = nullptr;
  }
}

bool AppListActivity::selectedIsPlugin() const {
  return selectedIndex_ >= 0 && selectedIndex_ < static_cast<int>(items_.size()) &&
         items_[static_cast<size_t>(selectedIndex_)].plugin;
}

void AppListActivity::selectIndex(const int index) {
  if (items_.empty()) return;
  selectedIndex_ = std::min(std::max(index, 0), static_cast<int>(items_.size()) - 1);
  M4FooterTouchPolicy::setMask(touchFooterButtonsMask());
  updateRequired_ = true;
}

void AppListActivity::moveSelection(const int delta) {
  if (items_.empty() || delta == 0) return;
  const int count = static_cast<int>(items_.size());
  int next = selectedIndex_ + delta;
  next %= count;
  if (next < 0) next += count;
  selectIndex(next);
}

void AppListActivity::activateBuiltin(const BuiltinAction action) {
  switch (action) {
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
  if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(items_.size())) return;
  const auto& item = items_[static_cast<size_t>(selectedIndex_)];
  if (!item.plugin) {
    activateBuiltin(item.builtin);
    return;
  }

  if (item.appIndex < 0 || item.appIndex >= static_cast<int>(apps_.size())) return;
  const auto app = apps_[static_cast<size_t>(item.appIndex)];
  if (renderingMutex_) xSemaphoreTake(renderingMutex_, portMAX_DELAY);
  auto onClosed = [this]() { requestExitSubActivity(); };
  if (app.runtime == M4xRuntimeKind::Native) {
    enterNewActivity(new NativeAppActivity(renderer, mappedInput, app, onClosed));
  } else {
    enterNewActivity(new AppRuntimeActivity(renderer, mappedInput, app, onClosed));
  }
  if (renderingMutex_) xSemaphoreGive(renderingMutex_);
}

void AppListActivity::openInstall() {
  if (renderingMutex_) xSemaphoreTake(renderingMutex_, portMAX_DELAY);
  enterNewActivity(new AppInstallActivity(renderer, mappedInput, "", [this]() {
    requestExitSubActivity();
  }));
  if (renderingMutex_) xSemaphoreGive(renderingMutex_);
}

void AppListActivity::uninstallSelected() {
  if (!selectedIsPlugin()) return;
  const auto& item = items_[static_cast<size_t>(selectedIndex_)];
  if (item.appIndex < 0 || item.appIndex >= static_cast<int>(apps_.size())) return;
  std::string err;
  if (!M4xInstaller::uninstall(apps_[static_cast<size_t>(item.appIndex)].id, uninstallClearData_, err)) {
    Serial.printf("[M4x] uninstall failed: %s\n", err.c_str());
  }
  reload();
  updateRequired_ = true;
}

void AppListActivity::loop() {
  if (subActivity) {
    if (pumpSubActivityFrame()) {
      reload();
      updateRequired_ = true;
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) {
    if (mode_ == 1) {
      mode_ = 0;
      M4FooterTouchPolicy::setMask(touchFooterButtonsMask());
      updateRequired_ = true;
    } else {
      onGoBack();
    }
    return;
  }

  const int count = static_cast<int>(items_.size());
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
          mode_ = 0;
          M4FooterTouchPolicy::setMask(touchFooterButtonsMask());
          updateRequired_ = true;
        } else {
          uninstallSelected();
        }
      } else if (uninstallDataToggleRect(renderer).contains(tx, ty)) {
        uninstallClearData_ = !uninstallClearData_;
        updateRequired_ = true;
      }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      uninstallSelected();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      uninstallClearData_ = !uninstallClearData_;
      updateRequired_ = true;
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
          mode_ = 1;
          M4FooterTouchPolicy::setMask(touchFooterButtonsMask());
          updateRequired_ = true;
        }
        return;
      }

      const int hit = layout.indexFromPoint(tx, ty);
      if (hit >= 0) {
        selectIndex(hit);
        if (selectedIsPlugin() && mappedInput.lastScreenTouchHeldMs() >= kAppLongPressMs) {
          mode_ = 1;
          M4FooterTouchPolicy::setMask(touchFooterButtonsMask());
          updateRequired_ = true;
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
        case MappedInputManager::SwipeDir::Up: moveSelection(-kDrawerColumns); break;
        case MappedInputManager::SwipeDir::Down: moveSelection(kDrawerColumns); break;
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
    mode_ = 1;
    M4FooterTouchPolicy::setMask(touchFooterButtonsMask());
    updateRequired_ = true;
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    openInstall();
    return;
  }

  if (count > 0) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      moveSelection(-kDrawerColumns);
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      moveSelection(kDrawerColumns);
    }
  }
}

void AppListActivity::render() const {
  renderer.clearScreen();
  const auto metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, L(Str::kApps));

  if (mode_ == 1 && selectedIsPlugin()) {
    const auto& item = items_[static_cast<size_t>(selectedIndex_)];
    const auto& app = apps_[static_cast<size_t>(item.appIndex)];
    const auto dialog = uninstallDialogLayout(renderer);
    const auto toggle = uninstallDataToggleRect(renderer);
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, 100, L(Str::kUninstallApp), true, EpdFontFamily::BOLD);
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 155, app.name.c_str());
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 195, "确认移除这个扩展应用？");
    renderer.fillRoundedRect(toggle.x, toggle.y, toggle.width, toggle.height, 10, Color::LightGray);
    M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, toggle.x, toggle.y, toggle.width, toggle.height,
                                uninstallClearData_ ? "同时清除数据：是" : "同时清除数据：否", true,
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
    const auto layout = makeDrawerGridLayout(renderer, selectedIndex_, static_cast<int>(items_.size()));
    for (int i = layout.pageStart; i < std::min(layout.itemCount, layout.pageStart + layout.pageItems); ++i) {
      const auto tile = layout.tileRect(i);
      const bool selected = i == selectedIndex_;
      if (selected) renderer.fillRoundedRect(tile.x, tile.y, tile.width, tile.height, 12, Color::LightGray);
      const auto& item = items_[static_cast<size_t>(i)];
      drawItemIcon(item, tile);

      // Character-count ellipsis, not pixel-width: four CJK glyphs must stay
      // intact (「文件管理」). Pixel truncate at tile.width-8 became 「文件管…」.
      const std::string label = utf8EllipsizeChars(item.label.c_str(), kDrawerLabelMaxChars);
      const int labelWidth = M4UiText::textWidth(renderer, UI_12_FONT_ID, label.c_str());
      const int labelY = tile.y + kDrawerIconSlot + 8;
      M4UiText::draw(renderer, UI_12_FONT_ID, tile.x + std::max(0, (tile.width - labelWidth) / 2), labelY,
                     label.c_str(), true);
    }
  }

  const bool pluginSelected = selectedIsPlugin();
  const auto labels = mode_ == 1 ? mappedInput.mapLabels(L(Str::kBackShort), L(Str::kConfirm), "", "")
                                 : mappedInput.mapLabels(L(Str::kBackShort), L(Str::kOpen),
                                                         pluginSelected ? L(Str::kUninstallApp) : "",
                                                         L(Str::kInstallApp));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
