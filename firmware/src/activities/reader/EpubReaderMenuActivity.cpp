#include "EpubReaderMenuActivity.h"

#include <EpdFontLoader.h>
#include <GfxRenderer.h>

#include "AutoPageTurnIntervalActivity.h"
#include "CrossPointSettings.h"
#include "I18n.h"
#include "MappedInputManager.h"
#include "activities/settings/FontSelectionActivity.h"
#include "activities/settings/SimpleBluetoothActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "managers/FontManager.h"
#include "util/M4ListTouchPolicy.h"
#include "util/M4ReaderMenuLayout.h"
#include "util/M4UiText.h"
#include "util/TouchHitGeometry.h"
#include <BluetoothHIDManager.h>
#include <algorithm>

namespace {
struct LayoutPreset {
  uint8_t top;
  uint8_t bottom;
  uint8_t left;
  uint8_t right;
  uint8_t lineSpacing;
};

constexpr LayoutPreset kCompactLayout{8, 8, 6, 6, 9};
constexpr LayoutPreset kStandardLayout{10, 10, 10, 10, 10};
constexpr LayoutPreset kRelaxedLayout{14, 14, 18, 18, 12};

constexpr int kOverlayTopBarH = M4ReaderMenuLayout::kOverlayTopBarH;
constexpr int kOverlayBottomBarH = M4ReaderMenuLayout::kOverlayBottomBarH;
constexpr int kStyleSheetH = M4ReaderMenuLayout::kStyleSheetH;
constexpr int kStyleSheetHeaderH = M4ReaderMenuLayout::kStyleSheetHeaderH;
constexpr int kTopBackHitW = 64;
constexpr int kTopBookmarkHitW = 64;
constexpr int kQuickFontMaxExternal = 3;
constexpr int kQuickFontGap = 8;
constexpr int kQuickFontSide = 20;
constexpr int kQuickFontH = 50;

bool matchesLayout(const LayoutPreset& p) {
  return SETTINGS.screenMargin_Top == p.top && SETTINGS.screenMargin_Bottom == p.bottom &&
         SETTINGS.screenMargin_Left == p.left && SETTINGS.screenMargin_Right == p.right &&
         SETTINGS.customLineSpacing == p.lineSpacing;
}

int quickIndexFromPoint(int x, int y, int width, int height) {
  if (y < height - kOverlayBottomBarH || y >= height || x < 0 || x >= width) return -1;
  const int cellW = std::max(1, width / 4);
  return std::min(3, x / cellW);
}

int styleIndexFromPoint(const M4ReaderMenuLayout::StylePanelLayout& L, int x, int y) {
  if (L.fontMinus.contains(x, y)) return 0;
  if (L.fontPlus.contains(x, y)) return 1;
  if (L.compact.contains(x, y)) return 2;
  if (L.standard.contains(x, y)) return 3;
  if (L.relaxed.contains(x, y)) return 4;
  return -1;
}

TouchHitGeometry::Rect quickFontRect(int pageWidth, int rowY, int count, int slot) {
  if (count <= 0 || slot < 0 || slot >= count) return {};
  const int innerW = std::max(80, pageWidth - kQuickFontSide * 2);
  const int cellW = std::max(48, (innerW - kQuickFontGap * (count - 1)) / count);
  const int usedW = cellW * count + kQuickFontGap * (count - 1);
  const int startX = kQuickFontSide + std::max(0, (innerW - usedW) / 2);
  return {startX + slot * (cellW + kQuickFontGap), rowY, cellW, kQuickFontH};
}

int quickFontSlotFromPoint(int pageWidth, int rowY, int count, int x, int y) {
  for (int i = 0; i < count; ++i) {
    if (quickFontRect(pageWidth, rowY, count, i).contains(x, y)) return i;
  }
  return -1;
}

void drawBackChevron(const GfxRenderer& renderer, int x, int cy) {
  for (int i = 0; i <= 8; ++i) {
    renderer.drawPixel(x + i, cy - i, true);
    renderer.drawPixel(x + i, cy - i + 1, true);
    renderer.drawPixel(x + i, cy + i, true);
    renderer.drawPixel(x + i, cy + i + 1, true);
  }
  renderer.fillRect(x + 7, cy, 18, 2, true);
}

void drawBookmarkGlyph(const GfxRenderer& renderer, int x, int y) {
  renderer.drawRect(x, y, 20, 27, true);
  renderer.fillRect(x + 3, y + 3, 14, 2, true);
  for (int i = 0; i < 7; ++i) {
    renderer.drawPixel(x + 3 + i, y + 22 + i / 2, false);
    renderer.drawPixel(x + 16 - i, y + 22 + i / 2, false);
  }
}
}  // namespace

void EpubReaderMenuActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();
  menuLayer_ = initialLayer_;
  moreSection_ = MoreSection::ROOT;
  selectedIndex = 0;
  readerStyleDirty_ = false;
  readerFontDirty_ = false;
  firstPaint_ = true;
  // Tab switches stay on FAST_REFRESH. HALF is only for returning from a
  // full-page child (font picker / interval / Bluetooth) onto More.
  forceHalfRefresh_ = false;
  if (menuLayer_ == MenuLayer::STYLE) {
    prepareQuickFontFamilies();
  }

  if (auto* host = getParentActivity(); host && !host->readerMenuSyncSupported()) {
    dataMenuItems.erase(
        std::remove_if(dataMenuItems.begin(), dataMenuItems.end(), [](const MenuItem& item) {
          return item.action == MenuAction::SYNC || item.action == MenuAction::SYNCY;
        }),
        dataMenuItems.end());
  }

  updateRequired = true;
  xTaskCreate(&EpubReaderMenuActivity::taskTrampoline, "EpubMenuTask", 8192, this, 1, &displayTaskHandle);
}

void EpubReaderMenuActivity::onExit() {
  ActivityWithSubactivity::onExit();
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void EpubReaderMenuActivity::taskTrampoline(void* param) {
  auto* self = static_cast<EpubReaderMenuActivity*>(param);
  self->displayTaskLoop();
}

void EpubReaderMenuActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired && !subActivity) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      std::string popup = std::move(pendingPopup_);
      renderScreen();
      if (!popup.empty()) GUI.drawPopup(renderer, popup.c_str());
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void EpubReaderMenuActivity::prepareQuickFontFamilies() {
  quickFontFamilies_.clear();
  const auto& available = FontManager::getInstance().getAvailableFamilies();
  const std::string current = SETTINGS.customFontFamily;

  if (!current.empty() && std::find(available.begin(), available.end(), current) != available.end()) {
    quickFontFamilies_.push_back(current);
  }
  for (const auto& family : available) {
    if (family == current) continue;
    quickFontFamilies_.push_back(family);
    if (static_cast<int>(quickFontFamilies_.size()) >= kQuickFontMaxExternal) break;
  }
}

void EpubReaderMenuActivity::applyQuickFontChoice(int slot) {
  bool changed = false;
  if (slot == 0) {
    if (SETTINGS.fontFamily != CrossPointSettings::SYSTEM_FONT) {
      SETTINGS.fontFamily = CrossPointSettings::SYSTEM_FONT;
      changed = true;
    }
  } else {
    const int index = slot - 1;
    if (index < 0 || index >= static_cast<int>(quickFontFamilies_.size())) return;
    const std::string& family = quickFontFamilies_[static_cast<size_t>(index)];
    if (SETTINGS.fontFamily != CrossPointSettings::FONT_CUSTOM ||
        std::string(SETTINGS.customFontFamily) != family) {
      strncpy(SETTINGS.customFontFamily, family.c_str(), sizeof(SETTINGS.customFontFamily) - 1);
      SETTINGS.customFontFamily[sizeof(SETTINGS.customFontFamily) - 1] = '\0';
      SETTINGS.fontFamily = CrossPointSettings::FONT_CUSTOM;
      changed = true;
    }
  }
  if (!changed) return;

  SETTINGS.saveToFile();
  readerStyleDirty_ = true;
  readerFontDirty_ = true;
  updateRequired = true;
}

void EpubReaderMenuActivity::applyInternalAction(InternalAction action) {
  if (action == InternalAction::OPEN_STYLE) {
    // Font directory iteration uses the same SD handle as TTF rasterization.
    // Do it only while the menu display task is not mid-draw.
    if (renderingMutex) xSemaphoreTake(renderingMutex, portMAX_DELAY);
    prepareQuickFontFamilies();
    if (renderingMutex) xSemaphoreGive(renderingMutex);
    menuLayer_ = MenuLayer::STYLE;
    selectedIndex = 0;
    updateRequired = true;
    return;
  }
  if (action == InternalAction::OPEN_MORE) {
    menuLayer_ = MenuLayer::MORE;
    moreSection_ = MoreSection::ROOT;
    selectedIndex = 0;
    updateRequired = true;
    return;
  }

  if (action == InternalAction::OPEN_MORE_TYPOGRAPHY ||
      action == InternalAction::OPEN_MORE_TURNING ||
      action == InternalAction::OPEN_MORE_DISPLAY ||
      action == InternalAction::OPEN_MORE_CONTROL ||
      action == InternalAction::OPEN_MORE_DATA) {
    menuLayer_ = MenuLayer::MORE;
    if (action == InternalAction::OPEN_MORE_TYPOGRAPHY) moreSection_ = MoreSection::TYPOGRAPHY;
    else if (action == InternalAction::OPEN_MORE_TURNING) moreSection_ = MoreSection::TURNING;
    else if (action == InternalAction::OPEN_MORE_DISPLAY) moreSection_ = MoreSection::APPEARANCE;
    else if (action == InternalAction::OPEN_MORE_CONTROL) moreSection_ = MoreSection::CONTROL;
    else moreSection_ = MoreSection::DATA;
    selectedIndex = 0;
    updateRequired = true;
    return;
  }

  if (action == InternalAction::FONT_DECREASE || action == InternalAction::FONT_INCREASE) {
    const bool increase = action == InternalAction::FONT_INCREASE;
    const uint8_t oldSize = SETTINGS.getReaderPixelSize();
    const uint8_t target = increase ? CrossPointSettings::nextReaderPixelSize(oldSize)
                                   : CrossPointSettings::prevReaderPixelSize(oldSize);
    const bool changed = target != oldSize;
    if (changed) SETTINGS.setReaderPixelSize(target);

    if (changed) {
      readerStyleDirty_ = true;
      readerFontDirty_ = true;
      SETTINGS.saveToFile();
    }
    updateRequired = true;
    return;
  }

  const LayoutPreset* preset = nullptr;
  if (action == InternalAction::LAYOUT_COMPACT) preset = &kCompactLayout;
  else if (action == InternalAction::LAYOUT_STANDARD) preset = &kStandardLayout;
  else if (action == InternalAction::LAYOUT_RELAXED) preset = &kRelaxedLayout;

  if (preset) {
    if (!matchesLayout(*preset)) {
      SETTINGS.screenMargin_Top = preset->top;
      SETTINGS.screenMargin_Bottom = preset->bottom;
      SETTINGS.screenMargin_Left = preset->left;
      SETTINGS.screenMargin_Right = preset->right;
      SETTINGS.customLineSpacing = preset->lineSpacing;
      SETTINGS.saveToFile();
      readerStyleDirty_ = true;
    }
    updateRequired = true;
  }
}

std::string EpubReaderMenuActivity::currentFontSizeLabel() const {
  return std::to_string(SETTINGS.getReaderPixelSize()) + "px";
}

std::string EpubReaderMenuActivity::styleValueFor(InternalAction action) const {
  switch (action) {
    case InternalAction::FONT_DECREASE:
    case InternalAction::FONT_INCREASE:
      return currentFontSizeLabel();
    case InternalAction::LAYOUT_COMPACT:
      return matchesLayout(kCompactLayout) ? "当前" : "0.9倍";
    case InternalAction::LAYOUT_STANDARD:
      return matchesLayout(kStandardLayout) ? "当前" : "1.0倍";
    case InternalAction::LAYOUT_RELAXED:
      return matchesLayout(kRelaxedLayout) ? "当前" : "1.2倍";
    case InternalAction::OPEN_MORE_TYPOGRAPHY:
      return "字体 · 段落 · 页面";
    case InternalAction::OPEN_MORE_TURNING:
      return SETTINGS.autoPageTurnEnabled ? "自动开启" : "方向 · 自动";
    case InternalAction::OPEN_MORE_DISPLAY:
      return SETTINGS.textAntiAliasing ? "抗锯齿开" : "抗锯齿关";
    case InternalAction::OPEN_MORE_CONTROL:
      return SETTINGS.globalNextPageModeEnabled ? "全局下一页开" : "按键 · 快捷操作";
    case InternalAction::OPEN_MORE_DATA: {
      const bool hasSync = std::any_of(dataMenuItems.begin(), dataMenuItems.end(), [](const MenuItem& item) {
        return item.action == MenuAction::SYNC || item.action == MenuAction::SYNCY;
      });
      return hasSync ? "同步 · 缓存" : "缓存";
    }
    case InternalAction::OPEN_STYLE:
    case InternalAction::OPEN_MORE:
      return ">";
    case InternalAction::NONE:
    default:
      return "";
  }
}

void EpubReaderMenuActivity::notifyParentStyleChanged() {
  const bool styleDirty = readerStyleDirty_;
  const bool fontDirty = readerFontDirty_;
  readerStyleDirty_ = false;
  readerFontDirty_ = false;
  if (!styleDirty) return;

  if (fontDirty) {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    EpdFontLoader::loadFontsFromSd(renderer);
    xSemaphoreGive(renderingMutex);
  }

  if (auto* host = getParentActivity()) host->onReaderMenuStyleChanged();
}

void EpubReaderMenuActivity::showMoreRoot() {
  moreSection_ = MoreSection::ROOT;
  selectedIndex = 0;
  updateRequired = true;
}

const char* EpubReaderMenuActivity::moreSectionKey() const {
  switch (moreSection_) {
    case MoreSection::TYPOGRAPHY: return "typography";
    case MoreSection::TURNING: return "turning";
    case MoreSection::APPEARANCE: return "display";
    case MoreSection::CONTROL: return "control";
    case MoreSection::DATA: return "data";
    case MoreSection::ROOT:
    default: return "root";
  }
}

const char* EpubReaderMenuActivity::moreSectionTitle() const {
  switch (moreSection_) {
    case MoreSection::TYPOGRAPHY: return "排版与字体";
    case MoreSection::TURNING: return "翻页与自动";
    case MoreSection::APPEARANCE: return "显示";
    case MoreSection::CONTROL: return "操作控制";
    case MoreSection::DATA: return "数据与缓存";
    case MoreSection::ROOT:
    default: return "更多";
  }
}

void EpubReaderMenuActivity::closeToReader() {
  onBack(pendingOrientation);
  notifyParentStyleChanged();
}

void EpubReaderMenuActivity::loop() {
  if (subActivity) {
    // Deferred pump: never destroy a nested picker while its loop is on the stack.
    pumpSubActivityFrame();
    return;
  }

  const auto& items = activeMenuItems();
  if (items.empty()) return;

  if (mappedInput.hasTouch()) {
    if (mappedInput.wasBackGesture()) {
      if (menuLayer_ == MenuLayer::MORE && moreSection_ != MoreSection::ROOT) {
        showMoreRoot();
      } else {
        closeToReader();
      }
      return;
    }

    const int pageWidth = renderer.getScreenWidth();
    const int pageHeight = renderer.getScreenHeight();

    if (menuLayer_ == MenuLayer::QUICK) {
      int tx = 0, ty = 0;
      if (mappedInput.wasScreenTapped(tx, ty)) {
        if (ty < kOverlayTopBarH && tx < kTopBackHitW) {
          closeToReader();
          return;
        }
        if (ty < kOverlayTopBarH && tx >= pageWidth - kTopBookmarkHitW) {
          auto actionCallback = onAction;
          actionCallback(MenuAction::ADD_BOOKMARK);
          return;
        }
        const int hit = quickIndexFromPoint(tx, ty, pageWidth, pageHeight);
        if (hit >= 0) {
          selectedIndex = hit;
          goto activate_menu_item;
        }
        closeToReader();
        return;
      }

      int dx = 0, dy = 0;
      if (mappedInput.wasScreenTouchDown(dx, dy)) {
        const int hit = quickIndexFromPoint(dx, dy, pageWidth, pageHeight);
        if (hit >= 0 && selectedIndex != hit) {
          selectedIndex = hit;
          updateRequired = true;
        }
        return;
      }
    } else if (menuLayer_ == MenuLayer::STYLE) {
      const int toolbarTop = pageHeight - kOverlayBottomBarH;
      const int panelTop = std::max(kOverlayTopBarH + 40, toolbarTop - kStyleSheetH);
      const auto styleLayout = M4ReaderMenuLayout::makeStylePanelLayout(0, pageWidth, panelTop + kStyleSheetHeaderH);
      const int fontRowY = styleLayout.fontPicker.y + 24;
      const int fontChoiceCount = 1 + static_cast<int>(quickFontFamilies_.size());
      int tx = 0, ty = 0;
      if (mappedInput.wasScreenTapped(tx, ty)) {
        const int toolbarHit = quickIndexFromPoint(tx, ty, pageWidth, pageHeight);
        if (toolbarHit >= 0) {
          if (toolbarHit == 2) return;
          if (toolbarHit == 3) {
            applyInternalAction(InternalAction::OPEN_MORE);
            return;
          }
          const auto action = quickMenuItems[static_cast<size_t>(toolbarHit)].action;
          auto actionCallback = onAction;
          actionCallback(action);
          notifyParentStyleChanged();
          return;
        }
        if (ty < panelTop) {
          closeToReader();
          return;
        }
        const int fontSlot = quickFontSlotFromPoint(pageWidth, fontRowY, fontChoiceCount, tx, ty);
        if (fontSlot >= 0) {
          applyQuickFontChoice(fontSlot);
          return;
        }
        const int hit = styleIndexFromPoint(styleLayout, tx, ty);
        if (hit >= 0) {
          selectedIndex = hit;
          goto activate_menu_item;
        }
        return;
      }
      int dx = 0, dy = 0;
      if (mappedInput.wasScreenTouchDown(dx, dy)) {
        const int hit = styleIndexFromPoint(styleLayout, dx, dy);
        if (hit >= 0 && selectedIndex != hit) {
          selectedIndex = hit;
          updateRequired = true;
        }
        return;
      }
    } else {
      const auto metrics = UITheme::getInstance().getMetrics();
      const auto orientation = renderer.getOrientation();
      const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
      const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
      const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
      const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
      const int contentX = isLandscapeCw ? hintGutterWidth : 0;
      const int hintGutterHeight = isPortraitInverted ? 50 : 0;
      const int contentTop = hintGutterHeight + metrics.headerHeight + metrics.verticalSpacing;
      const int listTop = contentTop;
      const int listHeight = pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
      const int totalItems = static_cast<int>(items.size());
      const int pageItems = std::max(1, listHeight / metrics.listRowHeight);

      M4ListTouchPolicy::Event te{};
      const auto sw = mappedInput.wasSwipe();
      if (sw == MappedInputManager::SwipeDir::Up) te.swipe = M4ListTouchPolicy::Swipe::Up;
      else if (sw == MappedInputManager::SwipeDir::Down) te.swipe = M4ListTouchPolicy::Swipe::Down;
      else if (sw == MappedInputManager::SwipeDir::Left) te.swipe = M4ListTouchPolicy::Swipe::Left;
      else if (sw == MappedInputManager::SwipeDir::Right) te.swipe = M4ListTouchPolicy::Swipe::Right;

      int dx = 0, dy = 0, tx = 0, ty = 0;
      te = M4ListTouchPolicy::mergeFrame(false, te.swipe, mappedInput.wasScreenTouchDown(dx, dy), dx, dy,
                                         mappedInput.wasScreenTapped(tx, ty), tx, ty);
      M4ListTouchPolicy::ListLayout layout;
      layout.listTop = listTop;
      layout.listHeight = listHeight;
      layout.rowStep = metrics.listRowHeight;
      layout.itemCount = totalItems;
      layout.selectedIndex = selectedIndex;

      int hit = -1;
      const auto act = M4ListTouchPolicy::resolveList(te, layout, hit);
      if (act == M4ListTouchPolicy::Action::PageDown || act == M4ListTouchPolicy::Action::PageUp) {
        selectedIndex = M4ListTouchPolicy::applyPage(
            selectedIndex, totalItems, pageItems, act == M4ListTouchPolicy::Action::PageDown);
        updateRequired = true;
        return;
      }
      if (act == M4ListTouchPolicy::Action::Select && hit >= 0) {
        if (selectedIndex != hit) {
          selectedIndex = hit;
          updateRequired = true;
        }
        return;
      }
      if (act == M4ListTouchPolicy::Action::Activate && hit >= 0) {
        selectedIndex = hit;
        goto activate_menu_item;
      }
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
      mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    selectedIndex = (selectedIndex + static_cast<int>(items.size()) - 1) % static_cast<int>(items.size());
    updateRequired = true;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
             mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    selectedIndex = (selectedIndex + 1) % static_cast<int>(items.size());
    updateRequired = true;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
  activate_menu_item:
    const auto selectedItem = items[static_cast<size_t>(selectedIndex)];

    if (selectedItem.internalAction != InternalAction::NONE) {
      applyInternalAction(selectedItem.internalAction);
      return;
    }

    const auto selectedAction = selectedItem.action;
    if (selectedAction == MenuAction::ROTATE_SCREEN) {
      pendingOrientation = (pendingOrientation + 1) % orientationLabels.size();
      updateRequired = true;
      return;
    }

    if (selectedAction == MenuAction::AUTO_PAGE_TURN) {
      SETTINGS.autoPageTurnEnabled = SETTINGS.autoPageTurnEnabled ? 0 : 1;
      SETTINGS.saveToFile();
      updateRequired = true;
      return;
    }

    if (selectedAction == MenuAction::TOGGLE_ANTI_ALIAS) {
      SETTINGS.textAntiAliasing = SETTINGS.textAntiAliasing ? 0 : 1;
      SETTINGS.saveToFile();
      updateRequired = true;
      return;
    }

    if (selectedAction == MenuAction::TOGGLE_DARK_MODE) {
      SETTINGS.epubDarkMode = SETTINGS.epubDarkMode ? 0 : 1;
      SETTINGS.saveToFile();
      updateRequired = true;
      return;
    }

    if (selectedAction == MenuAction::TOGGLE_FONT) {
      SETTINGS.fontFamily = SETTINGS.fontFamily == CrossPointSettings::SYSTEM_FONT
                                ? CrossPointSettings::FONT_CUSTOM
                                : CrossPointSettings::SYSTEM_FONT;
      SETTINGS.saveToFile();
      readerStyleDirty_ = true;
      readerFontDirty_ = true;
      updateRequired = true;
      return;
    }

    if (selectedAction == MenuAction::SELECT_EXTERNAL_FONT) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      enterNewActivity(new FontSelectionActivity(renderer, mappedInput, [this](bool loaded) {
        exitActivity();
        if (loaded) {
          readerStyleDirty_ = true;
          readerFontDirty_ = true;
        } else {
          xSemaphoreTake(renderingMutex, portMAX_DELAY);
          pendingPopup_ = L(Str::kFontLoadFailed);
          xSemaphoreGive(renderingMutex);
        }
        forceHalfRefresh_ = true;
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
      return;
    }

    if (selectedAction == MenuAction::TOGGLE_GLOBAL_NEXT_PAGE) {
      SETTINGS.globalNextPageModeEnabled = SETTINGS.globalNextPageModeEnabled ? 0 : 1;
      SETTINGS.saveToFile();
      updateRequired = true;
      return;
    }

    if (selectedAction == MenuAction::PAGE_TURN_MODE) {
      SETTINGS.autoPageTurnMode = SETTINGS.autoPageTurnMode ? 0 : 1;
      SETTINGS.saveToFile();
      updateRequired = true;
      return;
    }

    if (selectedAction == MenuAction::PAGE_TURN_INTERVAL) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      enterNewActivity(new AutoPageTurnIntervalActivity(
          renderer, mappedInput, SETTINGS.autoPageTurnInterval,
          [this](const int interval) {
            SETTINGS.autoPageTurnInterval = interval;
            SETTINGS.saveToFile();
            exitActivity();
            forceHalfRefresh_ = true;
            updateRequired = true;
          },
          [this]() {
            exitActivity();
            forceHalfRefresh_ = true;
            updateRequired = true;
          }));
      xSemaphoreGive(renderingMutex);
      return;
    }

#ifdef CROSSPOINT_X3
    if (selectedAction == MenuAction::TILT_PAGE_TURN) {
      SETTINGS.tiltPageTurnEnabled = SETTINGS.tiltPageTurnEnabled ? 0 : 1;
      SETTINGS.saveToFile();
      updateRequired = true;
      return;
    }

    if (selectedAction == MenuAction::TILT_PAGE_TURN_SETTINGS) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      enterNewActivity(new TiltPageTurnSettingsActivity(renderer, mappedInput, [this]() {
        exitActivity();
        forceHalfRefresh_ = true;
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
      return;
    }
#endif

    if (selectedAction == MenuAction::LONG_PRESS_CONFIRM_MAPPING) {
#ifdef CROSSPOINT_X3
      constexpr uint8_t maxAction = 6;
#else
      constexpr uint8_t maxAction = 5;
#endif
      SETTINGS.longPressConfirmAction = (SETTINGS.longPressConfirmAction + 1) % (maxAction + 1);
      SETTINGS.saveToFile();
      updateRequired = true;
      return;
    }

    if (selectedAction == MenuAction::BLUETOOTH_SETTINGS) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      enterNewActivity(new SimpleBluetoothActivity(renderer, mappedInput, [this]() {
        exitActivity();
        forceHalfRefresh_ = true;
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
      return;
    }

    auto actionCallback = onAction;
    actionCallback(selectedAction);
    if (selectedAction != MenuAction::ADD_BOOKMARK) notifyParentStyleChanged();
    return;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (menuLayer_ == MenuLayer::MORE && moreSection_ != MoreSection::ROOT) {
      showMoreRoot();
    } else {
      closeToReader();
    }
    return;
  }
}

void EpubReaderMenuActivity::renderScreen() {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto metrics = UITheme::getInstance().getMetrics();

  auto drawChip = [this](const TouchHitGeometry::Rect& r, const std::string& label,
                         bool selected, bool active) {
    renderer.fillRect(r.x, r.y, r.width, r.height, false);
    renderer.drawRect(r.x, r.y, r.width, r.height, true);
    if (selected) {
      renderer.drawRect(r.x + 2, r.y + 2, std::max(1, r.width - 4), std::max(1, r.height - 4), true);
    } else if (active && r.width > 6 && r.height > 6) {
      renderer.fillRect(r.x + 6, r.y + 10, 3, std::max(8, r.height - 20), true);
    }
    M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, r.x, r.y, r.width, r.height,
                                label.c_str(), true,
                                (selected || active) ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR, 8);
  };

  if (menuLayer_ == MenuLayer::QUICK) {
    renderer.fillRect(0, 0, pageWidth, kOverlayTopBarH, false);
    renderer.fillRect(0, pageHeight - kOverlayBottomBarH, pageWidth, kOverlayBottomBarH, false);
    renderer.drawLine(0, kOverlayTopBarH - 1, pageWidth - 1, kOverlayTopBarH - 1, true);
    renderer.drawLine(0, pageHeight - kOverlayBottomBarH, pageWidth - 1,
                      pageHeight - kOverlayBottomBarH, true);

    const int topCy = kOverlayTopBarH / 2;
    drawBackChevron(renderer, 16, topCy);

    const int titleX = 56;
    const int titleW = std::max(60, pageWidth - 56 - 126);
    const std::string titleText = M4UiText::truncated(
        renderer, UI_10_FONT_ID, title.c_str(), titleW, EpdFontFamily::BOLD);
    M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, titleX, 0, titleW, kOverlayTopBarH,
                                titleText.c_str(), true, EpdFontFamily::BOLD, 8);

    const std::string progressText = std::to_string(std::max(0, std::min(bookProgressPercent, 100))) + "%";
    M4UiText::draw(renderer, UI_10_FONT_ID, pageWidth - 114, 20, progressText.c_str(), true,
                   EpdFontFamily::REGULAR);
    drawBookmarkGlyph(renderer, pageWidth - 38, 15);

    const int barTop = pageHeight - kOverlayBottomBarH;
    const int cellW = std::max(1, pageWidth / 4);
    for (int i = 0; i < 4; ++i) {
      const int x = i * cellW;
      const int w = (i == 3) ? pageWidth - x : cellW;
      const bool selected = selectedIndex == i;
      const int cx = x + w / 2;
      const int iconY = barTop + 22;

      if (selected) {
        renderer.fillRect(x + 12, barTop + 5, std::max(1, w - 24), 3, true);
      }
      if (i == 0) {
        renderer.fillRect(cx - 13, iconY - 8, 26, 2, true);
        renderer.fillRect(cx - 13, iconY, 21, 2, true);
        renderer.fillRect(cx - 13, iconY + 8, 26, 2, true);
      } else if (i == 1) {
        renderer.drawRect(cx - 12, iconY - 9, 24, 18, true);
        renderer.fillRect(cx - 2, iconY - 13, 4, 26, false);
        renderer.fillRect(cx - 1, iconY - 6, 2, 12, true);
      } else if (i == 2) {
        M4UiText::drawCenteredInBox(renderer, UI_12_FONT_ID, x, iconY - 16, w, 32, "A", true,
                                    EpdFontFamily::BOLD, 8);
      } else {
        renderer.fillRect(cx - 14, iconY - 2, 4, 4, true);
        renderer.fillRect(cx - 2, iconY - 2, 4, 4, true);
        renderer.fillRect(cx + 10, iconY - 2, 4, 4, true);
      }

      M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, x, barTop + 48, w, 34,
                                  quickMenuItems[static_cast<size_t>(i)].label.c_str(), true,
                                  selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR, 8);
    }

    firstPaint_ = false;
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  if (menuLayer_ == MenuLayer::STYLE) {
    const int toolbarTop = pageHeight - kOverlayBottomBarH;
    const int panelTop = std::max(kOverlayTopBarH + 40, toolbarTop - kStyleSheetH);
    const int panelH = std::max(0, toolbarTop - panelTop);
    renderer.fillRect(0, panelTop, pageWidth, panelH, false);
    renderer.drawLine(0, panelTop, pageWidth - 1, panelTop, true);
    renderer.fillRect(pageWidth / 2 - 18, panelTop + 8, 36, 2, true);
    M4UiText::draw(renderer, UI_10_FONT_ID, 20, panelTop + 27, "字体", true, EpdFontFamily::BOLD);
    M4UiText::draw(renderer, UI_10_FONT_ID, pageWidth - 34, panelTop + 27, "×", true, EpdFontFamily::REGULAR);

    const auto L = M4ReaderMenuLayout::makeStylePanelLayout(0, pageWidth, panelTop + kStyleSheetHeaderH);
    M4UiText::draw(renderer, UI_10_FONT_ID, L.labelX, L.fontLabelY, "字号", true, EpdFontFamily::BOLD);
    M4UiText::draw(renderer, UI_10_FONT_ID, L.labelX, L.layoutLabelY, "行距", true, EpdFontFamily::BOLD);

    const auto fontGroup = L.fontGroupRect();
    renderer.fillRect(fontGroup.x, fontGroup.y, fontGroup.width, fontGroup.height, false);
    renderer.drawRect(fontGroup.x, fontGroup.y, fontGroup.width, fontGroup.height, true);
    renderer.fillRect(L.fontValue.x, fontGroup.y, 1, fontGroup.height, true);
    renderer.fillRect(L.fontPlus.x, fontGroup.y, 1, fontGroup.height, true);
    if (selectedIndex == 0) renderer.drawRect(L.fontMinus.x + 2, L.fontMinus.y + 2,
                                              L.fontMinus.width - 4, L.fontMinus.height - 4, true);
    if (selectedIndex == 1) renderer.drawRect(L.fontPlus.x + 2, L.fontPlus.y + 2,
                                              L.fontPlus.width - 4, L.fontPlus.height - 4, true);
    M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, L.fontMinus.x, L.fontMinus.y,
                                L.fontMinus.width, L.fontMinus.height, "A-", true,
                                selectedIndex == 0 ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR, 8);
    M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, L.fontValue.x, L.fontValue.y,
                                L.fontValue.width, L.fontValue.height, currentFontSizeLabel().c_str(), true,
                                EpdFontFamily::BOLD, 8);
    M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, L.fontPlus.x, L.fontPlus.y,
                                L.fontPlus.width, L.fontPlus.height, "A+", true,
                                selectedIndex == 1 ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR, 8);

    drawChip(L.compact, "紧凑", selectedIndex == 2, matchesLayout(kCompactLayout));
    drawChip(L.standard, "标准", selectedIndex == 3, matchesLayout(kStandardLayout));
    drawChip(L.relaxed, "宽松", selectedIndex == 4, matchesLayout(kRelaxedLayout));

    const int fontLabelY = L.fontPicker.y;
    const int fontRowY = fontLabelY + 24;
    M4UiText::draw(renderer, UI_10_FONT_ID, kQuickFontSide, fontLabelY, "字体", true, EpdFontFamily::BOLD);
    const int fontChoiceCount = 1 + static_cast<int>(quickFontFamilies_.size());
    for (int slot = 0; slot < fontChoiceCount; ++slot) {
      const auto r = quickFontRect(pageWidth, fontRowY, fontChoiceCount, slot);
      std::string label = slot == 0 ? "系统" : quickFontFamilies_[static_cast<size_t>(slot - 1)];
      label = M4UiText::truncated(renderer, UI_10_FONT_ID, label.c_str(), std::max(30, r.width - 16),
                                  EpdFontFamily::REGULAR);
      const bool active = slot == 0
                              ? SETTINGS.fontFamily == CrossPointSettings::SYSTEM_FONT
                              : SETTINGS.fontFamily == CrossPointSettings::FONT_CUSTOM &&
                                    std::string(SETTINGS.customFontFamily) ==
                                        quickFontFamilies_[static_cast<size_t>(slot - 1)];
      drawChip(r, label, false, active);
    }

    renderer.fillRect(0, toolbarTop, pageWidth, kOverlayBottomBarH, false);
    renderer.drawLine(0, toolbarTop, pageWidth - 1, toolbarTop, true);
    const int cellW = std::max(1, pageWidth / 4);
    for (int i = 0; i < 4; ++i) {
      const int x = i * cellW;
      const int w = (i == 3) ? pageWidth - x : cellW;
      const int cx = x + w / 2;
      const int iconY = toolbarTop + 22;
      const bool active = i == 2;

      if (active) renderer.fillRect(x + 12, toolbarTop + 5, std::max(1, w - 24), 3, true);
      if (i == 0) {
        renderer.fillRect(cx - 13, iconY - 8, 26, 2, true);
        renderer.fillRect(cx - 13, iconY, 21, 2, true);
        renderer.fillRect(cx - 13, iconY + 8, 26, 2, true);
      } else if (i == 1) {
        renderer.drawRect(cx - 12, iconY - 9, 24, 18, true);
        renderer.fillRect(cx - 2, iconY - 13, 4, 26, false);
        renderer.fillRect(cx - 1, iconY - 6, 2, 12, true);
      } else if (i == 2) {
        M4UiText::drawCenteredInBox(renderer, UI_12_FONT_ID, x, iconY - 16, w, 32, "A", true,
                                    EpdFontFamily::BOLD, 8);
      } else {
        renderer.fillRect(cx - 14, iconY - 2, 4, 4, true);
        renderer.fillRect(cx - 2, iconY - 2, 4, 4, true);
        renderer.fillRect(cx + 10, iconY - 2, 4, 4, true);
      }

      M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, x, toolbarTop + 48, w, 34,
                                  quickMenuItems[static_cast<size_t>(i)].label.c_str(), true,
                                  active ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR, 8);
    }

    firstPaint_ = false;
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  renderer.clearScreen();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;

  const std::string headerTitle = moreSection_ == MoreSection::ROOT
                                      ? title + " · 更多"
                                      : std::string(moreSectionTitle());
  const std::string truncTitle =
      M4UiText::truncated(renderer, UI_12_FONT_ID, headerTitle.c_str(), contentWidth - 40, EpdFontFamily::BOLD);
  GUI.drawHeader(renderer, Rect{contentX, hintGutterHeight, contentWidth, metrics.headerHeight}, truncTitle.c_str());

  const int contentTop = hintGutterHeight + metrics.headerHeight + metrics.verticalSpacing;
  const int listTop = contentTop;
  const int listHeight = pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const auto& items = activeMenuItems();
  const int totalItems = static_cast<int>(items.size());

  GUI.drawList(
      renderer, Rect{contentX, listTop, contentWidth, listHeight}, totalItems, selectedIndex,
      [&items](int index) -> std::string { return items[static_cast<size_t>(index)].label; },
      nullptr, nullptr,
      [this, &items](int index) -> std::string {
        const auto& item = items[static_cast<size_t>(index)];
        if (item.internalAction != InternalAction::NONE) return styleValueFor(item.internalAction);
        const auto action = item.action;
        if (action == MenuAction::ROTATE_SCREEN) return std::string(orientationLabels[pendingOrientation]);
        if (action == MenuAction::AUTO_PAGE_TURN) return std::string(autoPageTurnLabels[SETTINGS.autoPageTurnEnabled ? 1 : 0]);
        if (action == MenuAction::TOGGLE_ANTI_ALIAS) return std::string(antiAliasLabels[SETTINGS.textAntiAliasing ? 1 : 0]);
        if (action == MenuAction::TOGGLE_DARK_MODE) return std::string(darkModeLabels[SETTINGS.epubDarkMode ? 1 : 0]);
        if (action == MenuAction::TOGGLE_FONT) return std::string(fontLabels[SETTINGS.fontFamily == CrossPointSettings::FONT_CUSTOM ? 1 : 0]);
        if (action == MenuAction::SELECT_EXTERNAL_FONT) {
          const char* fontName = getExternalFontName();
          return strlen(fontName) > 0 ? std::string(fontName) : ">";
        }
        if (action == MenuAction::TOGGLE_GLOBAL_NEXT_PAGE) return std::string(globalNextPageLabels[SETTINGS.globalNextPageModeEnabled ? 1 : 0]);
#ifdef CROSSPOINT_X3
        if (action == MenuAction::TILT_PAGE_TURN) return std::string(tiltPageTurnLabels[SETTINGS.tiltPageTurnEnabled ? 1 : 0]);
#endif
        if (action == MenuAction::PAGE_TURN_MODE) return std::string(pageTurnModeLabels[SETTINGS.autoPageTurnMode ? 1 : 0]);
        if (action == MenuAction::PAGE_TURN_INTERVAL) return std::to_string(SETTINGS.autoPageTurnInterval) + "秒";
        if (action == MenuAction::LONG_PRESS_CONFIRM_MAPPING) {
          const uint8_t actionIdx = SETTINGS.longPressConfirmAction;
          const char* value = actionIdx < longPressConfirmLabels.size() ? longPressConfirmLabels[actionIdx] : "";
          return std::string(value);
        }
        if (action == MenuAction::BLUETOOTH_SETTINGS) {
          try {
            auto& btMgr = BluetoothHIDManager::getInstance();
            return btMgr.isEnabled() ? "已开启" : "已关闭";
          } catch (...) {
            return "错误";
          }
        }
        return ">";
      });

  const auto labels = mappedInput.mapLabels(
      menuLayer_ == MenuLayer::MORE && moreSection_ != MoreSection::ROOT ? "« 更多" : "« 阅读",
      "选择", "向上", "向下");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (forceHalfRefresh_) {
    forceHalfRefresh_ = false;
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
  firstPaint_ = false;
}
