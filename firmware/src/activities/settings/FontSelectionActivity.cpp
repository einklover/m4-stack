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

namespace {
struct FontGridLayout {
  int columns = 2;
  int rows = 1;
  int itemsPerPage = 2;
  int startX = 0;
  int startY = 0;
  int cellWidth = 0;
  int cellHeight = 48;
  int gap = 8;
  int areaHeight = 0;

  TouchHitGeometry::Rect cellRect(int slot) const {
    if (slot < 0 || slot >= itemsPerPage) return {};
    const int row = slot / columns;
    const int col = slot % columns;
    return {startX + col * (cellWidth + gap), startY + row * (cellHeight + gap), cellWidth, cellHeight};
  }

  int slotFromPoint(int x, int y) const {
    for (int slot = 0; slot < itemsPerPage; ++slot) {
      if (cellRect(slot).contains(x, y)) return slot;
    }
    return -1;
  }
};

FontGridLayout makeFontGridLayout(int screenWidth, int screenHeight, int headerHeight, int buttonHintsHeight) {
  FontGridLayout L;
  constexpr int side = 20;
  constexpr int topGap = 14;
  constexpr int bottomGap = 12;

  // M4 landscape has enough width for three generous touch targets. Portrait
  // keeps two columns so family names remain readable and taps stay forgiving.
  L.columns = screenWidth >= 640 ? 3 : 2;
  L.startX = side;
  L.startY = headerHeight + topGap;
  const int usableWidth = std::max(120, screenWidth - side * 2);
  L.cellWidth = std::max(70, (usableWidth - L.gap * (L.columns - 1)) / L.columns);
  L.areaHeight = std::max(L.cellHeight, screenHeight - L.startY - buttonHintsHeight - bottomGap);
  L.rows = std::max(1, (L.areaHeight + L.gap) / (L.cellHeight + L.gap));
  L.itemsPerPage = std::max(1, L.columns * L.rows);
  return L;
}
}  // namespace

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
  selectedIndex = 0;
  scrollOffset = 0;
  for (size_t i = 0; i < fontFamilies.size(); i++) {
    if (fontFamilies[i] == current) {
      selectedIndex = static_cast<int>(i);
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

  const auto metrics = UITheme::getInstance().getMetrics();
  const auto grid = makeFontGridLayout(renderer.getScreenWidth(), renderer.getScreenHeight(),
                                       metrics.headerHeight, metrics.buttonHintsHeight);
  itemsPerPage = grid.itemsPerPage;

  // Touch: stable paged grid. A touch-down only focuses; the tap activates.
  // This avoids loading a TTF while the finger is still exploring the grid.
  if (mappedInput.hasTouch() && !fontFamilies.empty()) {
    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
      const int pageStart = (selectedIndex / itemsPerPage) * itemsPerPage;
      const int slot = selectedIndex - pageStart;
      int targetPage = pageStart;
      if (swipe == MappedInputManager::SwipeDir::Up) {
        if (pageStart + itemsPerPage < static_cast<int>(fontFamilies.size())) {
          targetPage = pageStart + itemsPerPage;
        }
      } else if (pageStart > 0) {
        targetPage = std::max(0, pageStart - itemsPerPage);
      }
      const int target = std::min(static_cast<int>(fontFamilies.size()) - 1, targetPage + slot);
      if (target != selectedIndex) {
        selectedIndex = target;
        update = true;
      }
    } else {
      int tx = 0, ty = 0;
      if (mappedInput.wasScreenTouchDown(tx, ty)) {
        const int slot = grid.slotFromPoint(tx, ty);
        const int hit = slot < 0 ? -1 : scrollOffset + slot;
        if (hit >= 0 && hit < static_cast<int>(fontFamilies.size()) && selectedIndex != hit) {
          selectedIndex = hit;
          update = true;
        }
      }
      if (mappedInput.wasScreenTapped(tx, ty)) {
        const int slot = grid.slotFromPoint(tx, ty);
        const int hit = slot < 0 ? -1 : scrollOffset + slot;
        if (hit >= 0 && hit < static_cast<int>(fontFamilies.size())) {
          selectedIndex = hit;
          saveAndExit();
          return;
        }
      }
    }
  }

  if (!fontFamilies.empty()) {
    const int count = static_cast<int>(fontFamilies.size());
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      selectedIndex = selectedIndex > 0 ? selectedIndex - 1 : count - 1;
      update = true;
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      selectedIndex = selectedIndex + 1 < count ? selectedIndex + 1 : 0;
      update = true;
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      int target = selectedIndex - grid.columns;
      if (target < 0) {
        const int col = selectedIndex % grid.columns;
        const int lastRowStart = ((count - 1) / grid.columns) * grid.columns;
        target = std::min(count - 1, lastRowStart + col);
      }
      selectedIndex = target;
      update = true;
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      int target = selectedIndex + grid.columns;
      if (target >= count) target = selectedIndex % grid.columns;
      if (target >= count) target = count - 1;
      selectedIndex = target;
      update = true;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    saveAndExit();
    return;
  }

  if (update) {
    scrollOffset = (selectedIndex / itemsPerPage) * itemsPerPage;
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
  const int scrollBarHeight = std::max(12, (areaHeight * itemsPerPage) / totalItems);
  const int totalPages = (totalItems + itemsPerPage - 1) / itemsPerPage;
  const int currentPage = scrollOffset / itemsPerPage;
  const int scrollBarY = startY + ((areaHeight - scrollBarHeight) * currentPage) / std::max(1, totalPages - 1);

  renderer.drawLine(scrollBarX, startY, scrollBarX, startY + areaHeight, true);
  renderer.fillRect(scrollBarX - metrics.scrollBarWidth, scrollBarY, metrics.scrollBarWidth, scrollBarHeight, true);
}

void FontSelectionActivity::render() const {
  renderer.clearScreen();

  const auto metrics = UITheme::getInstance().getMetrics();
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const auto grid = makeFontGridLayout(screenW, screenH, metrics.headerHeight, metrics.buttonHintsHeight);

  auto* self = const_cast<FontSelectionActivity*>(this);
  self->itemsPerPage = grid.itemsPerPage;
  if (!fontFamilies.empty()) {
    self->scrollOffset = (selectedIndex / grid.itemsPerPage) * grid.itemsPerPage;
  } else {
    self->scrollOffset = 0;
  }

  GUI.drawHeader(renderer, Rect{0, 0, screenW, metrics.headerHeight}, L(Str::kSelectFont));

  if (fontFamilies.empty()) {
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
      M4UiText::draw(renderer, UI_10_FONT_ID, boxX + (boxW - tw) / 2,
                     boxY + 12 + i * (lineH + lineSpacing), lines[i]);
    }
  } else {
    const std::string current = SETTINGS.customFontFamily;
    for (int slot = 0; slot < grid.itemsPerPage; ++slot) {
      const int index = scrollOffset + slot;
      if (index >= static_cast<int>(fontFamilies.size())) break;

      const auto r = grid.cellRect(slot);
      const bool selected = index == selectedIndex;
      const bool active = SETTINGS.fontFamily == CrossPointSettings::FONT_CUSTOM && fontFamilies[index] == current;

      if (selected) {
        renderer.fillRect(r.x, r.y, r.width, r.height, true);
      } else {
        renderer.drawRect(r.x, r.y, r.width, r.height, true);
        if (active && r.width > 6 && r.height > 6) {
          renderer.drawRect(r.x + 2, r.y + 2, r.width - 4, r.height - 4, true);
        }
      }

      const std::string label = M4UiText::truncated(renderer, UI_10_FONT_ID,
                                                    fontFamilies[index].c_str(), r.width - 16,
                                                    (selected || active) ? EpdFontFamily::BOLD
                                                                         : EpdFontFamily::REGULAR);
      M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, r.x, r.y, r.width, r.height,
                                  label.c_str(), !selected,
                                  (selected || active) ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR, 8);
    }
    drawScrollBar(static_cast<int>(fontFamilies.size()), grid.startY, grid.areaHeight);
  }

  const auto labels = mappedInput.mapLabels(L(Str::kBackShort), L(Str::kSelect), L(Str::kUp), L(Str::kDown));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
