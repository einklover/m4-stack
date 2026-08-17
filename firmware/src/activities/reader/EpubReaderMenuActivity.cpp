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

bool matchesLayout(const LayoutPreset& p) {
  return SETTINGS.screenMargin_Top == p.top && SETTINGS.screenMargin_Bottom == p.bottom &&
         SETTINGS.screenMargin_Left == p.left && SETTINGS.screenMargin_Right == p.right &&
         SETTINGS.customLineSpacing == p.lineSpacing;
}

int customAutoFontPx(uint8_t fontSize) {
  switch (fontSize) {
    case CrossPointSettings::SMALL:
      return 12;
    case CrossPointSettings::MEDIUM:
      return 14;
    case CrossPointSettings::LARGE:
      return 16;
    case CrossPointSettings::EXTRA_LARGE:
      return 18;
    default:
      return 16;
  }
}

int systemFontPx(uint8_t fontSize) {
  switch (fontSize) {
    case CrossPointSettings::SMALL:
      return 12;
    case CrossPointSettings::MEDIUM:
      return 16;
    case CrossPointSettings::LARGE:
    case CrossPointSettings::EXTRA_LARGE:
    default:
      return 18;
  }
}
}  // namespace

void EpubReaderMenuActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();
  menuLayer_ = MenuLayer::QUICK;
  selectedIndex = 0;
  readerStyleDirty_ = false;
  readerFontDirty_ = false;
  firstPaint_ = true;

  // The concrete reader host is authoritative for optional capabilities. TXT
  // deliberately has no remote-progress sync implementation, so those rows do
  // not exist there instead of leading to a "not supported" dead end.
  if (auto* host = getParentActivity(); host && !host->readerMenuSyncSupported()) {
    moreMenuItems.erase(
        std::remove_if(moreMenuItems.begin(), moreMenuItems.end(), [](const MenuItem& item) {
          return item.action == MenuAction::SYNC || item.action == MenuAction::SYNCY;
        }),
        moreMenuItems.end());
  }

  updateRequired = true;
  xTaskCreate(&EpubReaderMenuActivity::taskTrampoline, "EpubMenuTask", 4096, this, 1, &displayTaskHandle);
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

void EpubReaderMenuActivity::applyInternalAction(InternalAction action) {
  if (action == InternalAction::OPEN_STYLE) {
    menuLayer_ = MenuLayer::STYLE;
    selectedIndex = 0;
    updateRequired = true;
    return;
  }
  if (action == InternalAction::OPEN_MORE) {
    menuLayer_ = MenuLayer::MORE;
    selectedIndex = 0;
    updateRequired = true;
    return;
  }

  if (action == InternalAction::FONT_DECREASE || action == InternalAction::FONT_INCREASE) {
    const bool increase = action == InternalAction::FONT_INCREASE;
    bool changed = false;

    if (SETTINGS.fontFamily == CrossPointSettings::FONT_CUSTOM) {
      const int current = SETTINGS.customFontSize == 0
                              ? customAutoFontPx(SETTINGS.fontSize)
                              : std::max(12, std::min(48, static_cast<int>(SETTINGS.customFontSize)));
      const int target = std::max(12, std::min(48, current + (increase ? 2 : -2)));
      if (target != current) {
        SETTINGS.customFontSize = static_cast<uint8_t>(target);
        changed = true;
      }
    } else {
      const uint8_t oldSize = SETTINGS.fontSize;
      uint8_t newSize = oldSize;
      if (increase) {
        if (oldSize == CrossPointSettings::SMALL) newSize = CrossPointSettings::MEDIUM;
        else if (oldSize == CrossPointSettings::MEDIUM) newSize = CrossPointSettings::LARGE;
      } else {
        if (oldSize >= CrossPointSettings::LARGE) newSize = CrossPointSettings::MEDIUM;
        else if (oldSize == CrossPointSettings::MEDIUM) newSize = CrossPointSettings::SMALL;
      }
      if (newSize != oldSize) {
        SETTINGS.fontSize = newSize;
        changed = true;
      }
    }

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
  int px = 16;
  if (SETTINGS.fontFamily == CrossPointSettings::FONT_CUSTOM) {
    px = SETTINGS.customFontSize == 0
             ? customAutoFontPx(SETTINGS.fontSize)
             : std::max(12, std::min(48, static_cast<int>(SETTINGS.customFontSize)));
  } else {
    px = systemFontPx(SETTINGS.fontSize);
  }
  return std::to_string(px) + "px";
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

  // Style reflow is a semantic host operation, not a synthetic rotation event.
  if (auto* host = getParentActivity()) host->onReaderMenuStyleChanged();
}

void EpubReaderMenuActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  const auto& items = activeMenuItems();
  if (items.empty()) return;

  if (mappedInput.hasTouch()) {
    if (mappedInput.wasBackGesture()) {
      if (returnToQuickMenu()) return;
      // Let the reader's normal close callback capture its current state first;
      // ActivityWithSubactivity defers child teardown until this frame returns.
      onBack(pendingOrientation);
      notifyParentStyleChanged();
      return;
    }

    const auto metrics = UITheme::getInstance().getMetrics();
    const int pageWidth = renderer.getScreenWidth();
    const int pageHeight = renderer.getScreenHeight();
    const auto orientation = renderer.getOrientation();
    const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
    const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
    const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
    const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
    const int contentX = isLandscapeCw ? hintGutterWidth : 0;
    const int contentWidth = pageWidth - hintGutterWidth;
    const int hintGutterHeight = isPortraitInverted ? 50 : 0;
    const int contentTop = hintGutterHeight + metrics.headerHeight + metrics.verticalSpacing;

    if (menuLayer_ == MenuLayer::QUICK) {
      const auto quickLayout = M4ReaderMenuLayout::makeQuickPanelLayout(contentX, contentWidth, contentTop);
      int tx = 0, ty = 0;
      if (mappedInput.wasScreenTapped(tx, ty)) {
        const int hit = quickLayout.indexFromPoint(tx, ty);
        if (hit >= 0) {
          selectedIndex = hit;
          goto activate_menu_item;
        }
        return;
      }
      int dx = 0, dy = 0;
      if (mappedInput.wasScreenTouchDown(dx, dy)) {
        const int hit = quickLayout.indexFromPoint(dx, dy);
        if (hit >= 0 && selectedIndex != hit) {
          selectedIndex = hit;
          updateRequired = true;
        }
        return;
      }
    } else if (menuLayer_ == MenuLayer::STYLE) {
      const auto styleLayout = M4ReaderMenuLayout::makeStylePanelLayout(contentX, contentWidth, contentTop);
      int tx = 0, ty = 0;
      if (mappedInput.wasScreenTapped(tx, ty)) {
        const int hit = styleLayout.indexFromPoint(tx, ty);
        if (hit >= 0) {
          selectedIndex = hit;
          goto activate_menu_item;
        }
        return;
      }
      int dx = 0, dy = 0;
      if (mappedInput.wasScreenTouchDown(dx, dy)) {
        const int hit = styleLayout.indexFromPoint(dx, dy);
        if (hit >= 0 && selectedIndex != hit) {
          selectedIndex = hit;
          updateRequired = true;
        }
        return;
      }
    } else {
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
            updateRequired = true;
          },
          [this]() {
            exitActivity();
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
        updateRequired = true;
      }));
      xSemaphoreGive(renderingMutex);
      return;
    }

    // Parent callbacks take their current-page snapshots before style reflow.
    // ADD_BOOKMARK intentionally stays in the menu, so keep style dirty until a
    // later transition/back to avoid invalidating the reader behind the menu.
    auto actionCallback = onAction;
    actionCallback(selectedAction);
    if (selectedAction != MenuAction::ADD_BOOKMARK) notifyParentStyleChanged();
    return;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (returnToQuickMenu()) return;
    onBack(pendingOrientation);
    notifyParentStyleChanged();
    return;
  }
}

void EpubReaderMenuActivity::renderScreen() {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();
  const auto metrics = UITheme::getInstance().getMetrics();

  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;

  std::string headerTitle = title;
  if (menuLayer_ == MenuLayer::STYLE) headerTitle += " · 样式";
  else if (menuLayer_ == MenuLayer::MORE) headerTitle += " · 更多";
  const std::string truncTitle =
      M4UiText::truncated(renderer, UI_12_FONT_ID, headerTitle.c_str(), contentWidth - 40, EpdFontFamily::BOLD);
  GUI.drawHeader(renderer, Rect{contentX, hintGutterHeight, contentWidth, metrics.headerHeight}, truncTitle.c_str());

  const int contentTop = hintGutterHeight + metrics.headerHeight + metrics.verticalSpacing;

  auto drawChip = [this](const TouchHitGeometry::Rect& r, const std::string& label,
                         bool selected, bool active) {
    if (selected) {
      renderer.fillRect(r.x, r.y, r.width, r.height, true);
    } else {
      renderer.drawRect(r.x, r.y, r.width, r.height);
      if (active && r.width > 6 && r.height > 6) {
        renderer.drawRect(r.x + 2, r.y + 2, r.width - 4, r.height - 4);
      }
    }
    M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, r.x, r.y, r.width, r.height,
                                label.c_str(), !selected,
                                (selected || active) ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR, 8);
  };

  auto drawQuickGlyph = [this](const TouchHitGeometry::Rect& r, int index) {
    const int cx = r.x + r.width / 2;
    const int top = r.y + 15;
    auto bar = [this](int x, int y, int w, int h) { renderer.fillRect(x, y, w, h, true); };

    switch (index) {
      case 0:
        bar(cx - 15, top, 30, 2);
        bar(cx - 15, top + 8, 24, 2);
        bar(cx - 15, top + 16, 30, 2);
        break;
      case 1:
        bar(cx - 15, top + 4, 30, 2);
        bar(cx - 15, top + 12, 30, 2);
        bar(cx - 4, top, 2, 18);
        break;
      case 2:
        M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, r.x, top - 4, r.width, 28,
                                    "Aa", true, EpdFontFamily::BOLD, 8);
        break;
      case 3:
      case 4: {
        const int x = cx - 10;
        bar(x, top, 2, 22);
        bar(x + 18, top, 2, 22);
        bar(x, top, 20, 2);
        bar(x, top + 20, 7, 2);
        bar(x + 13, top + 20, 7, 2);
        if (index == 3) {
          bar(cx + 14, top + 5, 12, 2);
          bar(cx + 19, top, 2, 12);
        }
        break;
      }
      case 5:
        bar(cx - 13, top + 8, 4, 4);
        bar(cx - 2, top + 8, 4, 4);
        bar(cx + 9, top + 8, 4, 4);
        break;
      default:
        break;
    }
  };

  if (menuLayer_ == MenuLayer::QUICK) {
    const auto L = M4ReaderMenuLayout::makeQuickPanelLayout(contentX, contentWidth, contentTop);
    const int progress = std::max(0, std::min(bookProgressPercent, 100));
    GUI.drawProgressBar(renderer,
                        Rect{L.progressBar.x, L.progressBar.y, L.progressBar.width, L.progressBar.height},
                        static_cast<size_t>(progress), static_cast<size_t>(100));
    for (int i = 0; i < static_cast<int>(quickMenuItems.size()); ++i) {
      const auto r = L.actionRect(i);
      const bool selected = selectedIndex == i;
      renderer.drawRect(r.x, r.y, r.width, r.height);
      if (selected && r.width > 8 && r.height > 8) {
        renderer.drawRect(r.x + 2, r.y + 2, r.width - 4, r.height - 4);
        renderer.fillRect(r.x + 6, r.y + 12, 3, std::max(8, r.height - 24), true);
      }
      drawQuickGlyph(r, i);
      M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, r.x, r.y + r.height - 34,
                                  r.width, 28, quickMenuItems[static_cast<size_t>(i)].label.c_str(),
                                  true, selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR, 8);
    }
  } else if (menuLayer_ == MenuLayer::STYLE) {
    const auto L = M4ReaderMenuLayout::makeStylePanelLayout(contentX, contentWidth, contentTop);
    M4UiText::draw(renderer, UI_10_FONT_ID, L.labelX, L.fontLabelY, "字号", true, EpdFontFamily::BOLD);
    M4UiText::draw(renderer, UI_10_FONT_ID, L.labelX, L.layoutLabelY, "样式", true, EpdFontFamily::BOLD);

    const auto fontGroup = L.fontGroupRect();
    if (selectedIndex == 0) renderer.fillRect(L.fontMinus.x, L.fontMinus.y, L.fontMinus.width, L.fontMinus.height, true);
    if (selectedIndex == 1) renderer.fillRect(L.fontPlus.x, L.fontPlus.y, L.fontPlus.width, L.fontPlus.height, true);
    renderer.drawRect(fontGroup.x, fontGroup.y, fontGroup.width, fontGroup.height);
    renderer.fillRect(L.fontValue.x, fontGroup.y, 1, fontGroup.height, true);
    renderer.fillRect(L.fontPlus.x, fontGroup.y, 1, fontGroup.height, true);
    M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, L.fontMinus.x, L.fontMinus.y,
                                L.fontMinus.width, L.fontMinus.height, styleMenuItems[0].label.c_str(),
                                selectedIndex != 0, selectedIndex == 0 ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR, 8);
    M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, L.fontValue.x, L.fontValue.y,
                                L.fontValue.width, L.fontValue.height, currentFontSizeLabel().c_str(),
                                true, EpdFontFamily::BOLD, 8);
    M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, L.fontPlus.x, L.fontPlus.y,
                                L.fontPlus.width, L.fontPlus.height, styleMenuItems[1].label.c_str(),
                                selectedIndex != 1, selectedIndex == 1 ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR, 8);

    drawChip(L.compact, styleMenuItems[2].label, selectedIndex == 2, matchesLayout(kCompactLayout));
    drawChip(L.standard, styleMenuItems[3].label, selectedIndex == 3, matchesLayout(kStandardLayout));
    drawChip(L.relaxed, styleMenuItems[4].label, selectedIndex == 4, matchesLayout(kRelaxedLayout));
    drawChip(L.fontPicker, styleMenuItems[5].label, selectedIndex == 5, false);
    drawChip(L.details, styleMenuItems[6].label, selectedIndex == 6, false);
  } else {
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
          if (action == MenuAction::GO_TO_PERCENT) {
            return std::to_string(std::max(0, std::min(bookProgressPercent, 100))) + "%";
          }
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
  }

  const char* backHint = menuLayer_ == MenuLayer::QUICK ? "« 阅读" : "« 快捷";
  const auto labels = mappedInput.mapLabels(backHint, "选择", "向上", "向下");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (firstPaint_) {
    firstPaint_ = false;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}
