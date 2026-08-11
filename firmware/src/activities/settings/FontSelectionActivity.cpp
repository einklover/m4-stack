#include "FontSelectionActivity.h"
#include "util/M4UiText.h"

#include <EpdFontLoader.h>
#include <HardwareSerial.h>

#include <algorithm>

#include "../../CrossPointSettings.h"
#include "../../I18n.h"
#include "../../fontIds.h"
#include "../../managers/FontCacheManager.h"
#include "../../managers/FontManager.h"
#include "components/UITheme.h"
#include "util/TouchHitGeometry.h"

FontSelectionActivity::FontSelectionActivity(GfxRenderer& renderer, MappedInputManager& inputManager,
                                             std::function<void(bool)> onClose)
    : Activity("Font Selection", renderer, inputManager), onClose(onClose) {}

FontSelectionActivity::~FontSelectionActivity() {}

void FontSelectionActivity::onEnter() {
  Serial.println("[FSA] onEnter start");
  Activity::onEnter();
  FontManager::getInstance().invalidateScan();
  fontFamilies = FontManager::getInstance().getAvailableTtfFamilies();
  Serial.printf("[FSA] Got %d families\n", fontFamilies.size());

  std::string current = SETTINGS.customFontFamily;
  for (size_t i = 0; i < fontFamilies.size(); i++) {
    if (fontFamilies[i] == current) {
      selectedIndex = static_cast<int>(i);
      if (selectedIndex >= itemsPerPage) {
        scrollOffset = selectedIndex - itemsPerPage / 2;
        if (scrollOffset > (int)fontFamilies.size() - itemsPerPage) {
          scrollOffset = std::max(0, (int)fontFamilies.size() - itemsPerPage);
        }
      }
      break;
    }
  }
  render();
}

void FontSelectionActivity::loop() {
  bool update = false;

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) {
    onClose(true);
    return;
  }

  // Touch: list select/activate + swipe scroll (same geometry as render()).
  if (mappedInput.hasTouch()) {
    const auto metrics = UITheme::getInstance().getMetrics();
    const int listStartY = 50;
    const int rowHeight = metrics.listRowHeight;
    const int buttonHintsHeight = metrics.buttonHintsHeight;
    const int screenH = renderer.getScreenHeight();
    const int availableHeight = screenH - listStartY - buttonHintsHeight;
    itemsPerPage = std::max(1, availableHeight / rowHeight);

    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up && !fontFamilies.empty()) {
      selectedIndex = std::min((int)fontFamilies.size() - 1, selectedIndex + itemsPerPage);
      update = true;
    } else if (swipe == MappedInputManager::SwipeDir::Down && !fontFamilies.empty()) {
      selectedIndex = std::max(0, selectedIndex - itemsPerPage);
      update = true;
    } else {
      int tx = 0, ty = 0;
      if (mappedInput.wasScreenTouchDown(tx, ty) && !fontFamilies.empty()) {
        int hit = -1;
        // Page-relative list: pageStart = scrollOffset
        if (ty >= listStartY && ty < listStartY + availableHeight) {
          const int row = (ty - listStartY) / rowHeight;
          hit = scrollOffset + row;
          if (hit >= 0 && hit < (int)fontFamilies.size()) {
            if (selectedIndex != hit) {
              selectedIndex = hit;
              update = true;
            }
          }
        }
      }
      if (mappedInput.wasScreenTapped(tx, ty) && !fontFamilies.empty()) {
        if (ty >= listStartY && ty < listStartY + availableHeight) {
          const int row = (ty - listStartY) / rowHeight;
          const int hit = scrollOffset + row;
          if (hit >= 0 && hit < (int)fontFamilies.size()) {
            selectedIndex = hit;
            saveAndExit();
            return;
          }
        }
      }
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
      mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    selectedIndex = (selectedIndex > 0) ? (selectedIndex - 1) : ((int)fontFamilies.size() - 1);
    update = true;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
             mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    selectedIndex = (selectedIndex < (int)fontFamilies.size() - 1) ? (selectedIndex + 1) : 0;
    update = true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    saveAndExit();
    return;
  }

  if (update) {
    if (selectedIndex < scrollOffset) {
      scrollOffset = selectedIndex;
    } else if (selectedIndex >= scrollOffset + itemsPerPage) {
      scrollOffset = selectedIndex - itemsPerPage + 1;
    }
    render();
  }
}

void FontSelectionActivity::saveAndExit() {
  if (selectedIndex >= 0 && selectedIndex < (int)fontFamilies.size()) {
    const std::string previousFamily = SETTINGS.customFontFamily;
    const uint8_t previousMode = SETTINGS.fontFamily;
    const bool fontChanged = previousMode != CrossPointSettings::FONT_CUSTOM ||
                             previousFamily != fontFamilies[selectedIndex];

    if (!fontChanged) {
      onClose(true);
      return;
    }

    // Keep the old settings persisted until the new face has actually loaded.
    // The loader may tear down the old renderer aliases, so restore and reload
    // the previous selection if the candidate fails.
    Serial.printf("[FSA] Trying font: %s\n", fontFamilies[selectedIndex].c_str());
    GUI.drawPopup(renderer, L(Str::kLoadingFontPleaseWait));
    strncpy(SETTINGS.customFontFamily, fontFamilies[selectedIndex].c_str(), sizeof(SETTINGS.customFontFamily) - 1);
    SETTINGS.customFontFamily[sizeof(SETTINGS.customFontFamily) - 1] = '\0';
    SETTINGS.fontFamily = CrossPointSettings::FONT_CUSTOM;
    FontCacheManager::clearCache();

    if (EpdFontLoader::loadFontsFromSd(renderer)) {
      SETTINGS.saveToFile();
      Serial.printf("[FSA] Font loaded: %s\n", fontFamilies[selectedIndex].c_str());
      onClose(true);
      return;
    }

    strncpy(SETTINGS.customFontFamily, previousFamily.c_str(), sizeof(SETTINGS.customFontFamily) - 1);
    SETTINGS.customFontFamily[sizeof(SETTINGS.customFontFamily) - 1] = '\0';
    SETTINGS.fontFamily = previousMode;
    SETTINGS.saveToFile();
    EpdFontLoader::loadFontsFromSd(renderer);
    Serial.printf("[FSA] Font load failed; restored: %s\n", previousFamily.c_str());
    onClose(false);
    return;
  }
  onClose(true);
}

void FontSelectionActivity::drawScrollBar(int totalItems, int startY, int areaHeight) const {
  if (totalItems <= itemsPerPage) return;

  auto metrics = UITheme::getInstance().getMetrics();
  const int scrollBarX = renderer.getScreenWidth() - metrics.scrollBarRightOffset;
  const int scrollBarHeight = (areaHeight * itemsPerPage) / totalItems;
  const int totalPages = (totalItems + itemsPerPage - 1) / itemsPerPage;
  const int currentPage = scrollOffset / itemsPerPage;
  const int scrollBarY = startY + ((areaHeight - scrollBarHeight) * currentPage) / std::max(1, totalPages - 1);

  renderer.drawLine(scrollBarX, startY, scrollBarX, startY + areaHeight, true);
  renderer.fillRect(scrollBarX - metrics.scrollBarWidth, scrollBarY, metrics.scrollBarWidth, scrollBarHeight, true);
}

void FontSelectionActivity::render() const {
  renderer.clearScreen();

  M4UiText::drawCentered(renderer, UI_12_FONT_ID, 15, L(Str::kSelectFont), true, EpdFontFamily::BOLD);

  const auto metrics = UITheme::getInstance().getMetrics();
  const int listStartY = 50;
  const int rowHeight = metrics.listRowHeight;
  const int buttonHintsHeight = metrics.buttonHintsHeight;
  const int screenH = renderer.getScreenHeight();
  const int availableHeight = screenH - listStartY - buttonHintsHeight;
  const int rowFont = mappedInput.hasTouch() ? UI_12_FONT_ID : UI_10_FONT_ID;

  const_cast<FontSelectionActivity*>(this)->itemsPerPage = std::max(1, availableHeight / rowHeight);

  int y = listStartY;

  if (fontFamilies.empty()) {
    const int screenW = renderer.getScreenWidth();
    const int boxPadding = 20;
    const int boxW = screenW - 2 * boxPadding;
    const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
    const int lineSpacing = 8;
    const char* lines[] = {L(Str::kPleaseVisit), "Copy to SD:", "/FONT/*.ttf", L(Str::kCopyToFontsDir)};
    constexpr int lineCount = 4;
    const int boxH = lineCount * lineH + (lineCount - 1) * lineSpacing + 24;
    const int boxX = boxPadding;
    const int boxY = (screenH - boxH) / 2;
    renderer.drawRect(boxX, boxY, boxW, boxH, true);
    for (int i = 0; i < lineCount; i++) {
      const int tw = M4UiText::textWidth(renderer, UI_10_FONT_ID, lines[i]);
      M4UiText::draw(renderer, UI_10_FONT_ID, boxX + (boxW - tw) / 2, boxY + 12 + i * (lineH + lineSpacing), lines[i]);
    }
  } else {
    for (int i = 0; i < itemsPerPage; i++) {
      int index = scrollOffset + i;
      if (index >= (int)fontFamilies.size()) break;
      bool selected = (index == selectedIndex);
      if (selected) {
        renderer.fillRect(10, y - 2, renderer.getScreenWidth() - 20, rowHeight, true);
      }
      renderer.drawText(rowFont, 20, y + (rowHeight / 2) - 6, fontFamilies[index].c_str(), !selected);
      y += rowHeight;
    }
    drawScrollBar((int)fontFamilies.size(), listStartY, availableHeight);
  }

  const auto labels = mappedInput.mapLabels(L(Str::kBackShort), L(Str::kSelect), L(Str::kUp), L(Str::kDown));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
