#include "EpubReaderPercentSelectionActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>

#include <algorithm>
#include <string>

#include "MappedInputManager.h"
#include "fontIds.h"
#include "util/M4ReaderMenuLayout.h"
#include "util/M4UiText.h"
#include "util/TouchHitGeometry.h"

namespace {
constexpr int kSmallStep = 1;
constexpr int kLargeStep = 10;
constexpr int kToolbarHeight = M4ReaderMenuLayout::kOverlayBottomBarH;
constexpr int kStepCount = 4;
constexpr int kStepDeltas[kStepCount] = {-10, -1, 1, 10};
constexpr const char* kStepLabels[kStepCount] = {"-10%", "-1%", "+1%", "+10%"};
constexpr const char* kToolbarLabels[4] = {"目录", "进度", "字体", "更多"};

struct ProgressSheetLayout {
  TouchHitGeometry::Rect panel{};
  TouchHitGeometry::Rect close{};
  TouchHitGeometry::Rect value{};
  TouchHitGeometry::Rect track{};
  TouchHitGeometry::Rect trackHit{};
  TouchHitGeometry::Rect steps[kStepCount]{};
  TouchHitGeometry::Rect jump{};
};

ProgressSheetLayout makeProgressSheet(int width, int height) {
  ProgressSheetLayout L;
  const int toolbarTop = std::max(0, height - kToolbarHeight);
  // Same band and header as the font sheet so tab switches share one top edge
  // and Progress does not leave a blank strip under the title.
  const int top = std::max(M4ReaderMenuLayout::kOverlayTopBarH + 40,
                           toolbarTop - M4ReaderMenuLayout::kStyleSheetH);
  const int contentTop = top + M4ReaderMenuLayout::kStyleSheetHeaderH;
  L.panel = {0, top, width, std::max(0, toolbarTop - top)};
  L.close = {width - 64, top, 64, 52};
  L.value = {24, contentTop + 38, std::max(80, width - 48), 48};
  L.track = {24, contentTop + 94, std::max(80, width - 48), 10};
  L.trackHit = {24, contentTop + 78, std::max(80, width - 48), 42};

  constexpr int gap = 8;
  const int innerW = std::max(120, width - 48);
  const int stepW = std::max(48, (innerW - gap * (kStepCount - 1)) / kStepCount);
  const int usedW = stepW * kStepCount + gap * (kStepCount - 1);
  const int startX = 24 + std::max(0, (innerW - usedW) / 2);
  const int stepY = contentTop + 134;
  for (int i = 0; i < kStepCount; ++i) {
    L.steps[i] = {startX + i * (stepW + gap), stepY, stepW, 50};
  }

  const int jumpW = std::min(180, std::max(120, width / 3));
  L.jump = {(width - jumpW) / 2, contentTop + 200, jumpW, 52};
  return L;
}

int stepFromPoint(const ProgressSheetLayout& L, int x, int y) {
  for (int i = 0; i < kStepCount; ++i) {
    if (L.steps[i].contains(x, y)) return i;
  }
  return -1;
}

int percentFromPoint(const ProgressSheetLayout& L, int x, int y) {
  if (!L.trackHit.contains(x, y) || L.track.width <= 1) return -1;
  const int clampedX = std::max(L.track.x, std::min(L.track.x + L.track.width - 1, x));
  return ((clampedX - L.track.x) * 100 + (L.track.width - 1) / 2) / (L.track.width - 1);
}

int toolbarIndexFromPoint(int x, int y, int width, int height) {
  if (y < height - kToolbarHeight || y >= height || x < 0 || x >= width) return -1;
  const int cellW = std::max(1, width / 4);
  return std::min(3, x / cellW);
}

void drawToolbar(GfxRenderer& renderer, int width, int height) {
  const int barTop = height - kToolbarHeight;
  const int cellW = std::max(1, width / 4);
  renderer.fillRect(0, barTop, width, kToolbarHeight, false);
  renderer.drawLine(0, barTop, width - 1, barTop, true);

  for (int i = 0; i < 4; ++i) {
    const int x = i * cellW;
    const int w = (i == 3) ? width - x : cellW;
    const int cx = x + w / 2;
    const int iconY = barTop + 22;
    const bool active = i == 1;

    if (active) renderer.fillRect(x + 12, barTop + 5, std::max(1, w - 24), 3, true);
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
                                kToolbarLabels[i], true,
                                active ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR, 8);
  }
}
}  // namespace

void EpubReaderPercentSelectionActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();
  firstPaint = true;
  updateRequired = true;
  xTaskCreate(&EpubReaderPercentSelectionActivity::taskTrampoline, "EpubPercentSlider", 4096, this, 1,
              &displayTaskHandle);
}

void EpubReaderPercentSelectionActivity::onExit() {
  ActivityWithSubactivity::onExit();
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void EpubReaderPercentSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<EpubReaderPercentSelectionActivity*>(param);
  self->displayTaskLoop();
}

void EpubReaderPercentSelectionActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired && !subActivity) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      renderScreen();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void EpubReaderPercentSelectionActivity::adjustPercent(const int delta) {
  const int next = std::max(0, std::min(100, percent + delta));
  if (next == percent) return;
  percent = next;
  updateRequired = true;
}

void EpubReaderPercentSelectionActivity::loop() {
  if (subActivity) {
    pumpSubActivityFrame();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) {
    onCancel();
    return;
  }

  if (mappedInput.hasTouch()) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty)) {
      const int toolbarHit = toolbarIndexFromPoint(tx, ty, renderer.getScreenWidth(), renderer.getScreenHeight());
      if (toolbarHit >= 0) {
        // Progress stays put; other tabs replace this sheet via the parent.
        if (toolbarHit == 1 || !onToolbar) return;
        onToolbar(toolbarHit);
        return;
      }

      const auto L = makeProgressSheet(renderer.getScreenWidth(), renderer.getScreenHeight());
      if (!L.panel.contains(tx, ty) || L.close.contains(tx, ty)) {
        onCancel();
        return;
      }
      if (L.jump.contains(tx, ty)) {
        onSelect(percent);
        return;
      }

      const int step = stepFromPoint(L, tx, ty);
      if (step >= 0) {
        adjustPercent(kStepDeltas[step]);
        return;
      }

      const int tappedPercent = percentFromPoint(L, tx, ty);
      if (tappedPercent >= 0 && tappedPercent != percent) {
        percent = tappedPercent;
        updateRequired = true;
      }
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    onSelect(percent);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    adjustPercent(-kSmallStep);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    adjustPercent(kSmallStep);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    adjustPercent(kLargeStep);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    adjustPercent(-kLargeStep);
    return;
  }
}

void EpubReaderPercentSelectionActivity::renderScreen() {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto L = makeProgressSheet(pageWidth, pageHeight);

  renderer.fillRect(L.panel.x, L.panel.y, L.panel.width, L.panel.height, false);
  renderer.drawLine(0, L.panel.y, pageWidth - 1, L.panel.y, true);
  renderer.fillRect(pageWidth / 2 - 18, L.panel.y + 8, 36, 2, true);

  M4UiText::draw(renderer, UI_10_FONT_ID, 20, L.panel.y + 30, "进度", true, EpdFontFamily::BOLD);
  M4UiText::draw(renderer, UI_10_FONT_ID, pageWidth - 38, L.panel.y + 30, "×", true,
                 EpdFontFamily::REGULAR);

  const std::string percentText = std::to_string(percent) + "%";
  M4UiText::drawCenteredInBox(renderer, UI_12_FONT_ID, L.value.x, L.value.y,
                              L.value.width, L.value.height, percentText.c_str(), true,
                              EpdFontFamily::BOLD, 8);

  renderer.drawRect(L.track.x, L.track.y, L.track.width, L.track.height, true);
  const int innerWidth = std::max(0, L.track.width - 4);
  const int fillWidth = innerWidth * percent / 100;
  if (fillWidth > 0) {
    renderer.fillRect(L.track.x + 2, L.track.y + 2, fillWidth,
                      std::max(1, L.track.height - 4), true);
  }

  for (int i = 0; i < kStepCount; ++i) {
    const auto r = L.steps[i];
    renderer.drawRect(r.x, r.y, r.width, r.height, true);
    M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, r.x, r.y, r.width, r.height,
                                kStepLabels[i], true, EpdFontFamily::BOLD, 8);
  }

  renderer.drawRect(L.jump.x, L.jump.y, L.jump.width, L.jump.height, true);
  renderer.drawRect(L.jump.x + 2, L.jump.y + 2, L.jump.width - 4, L.jump.height - 4, true);
  const std::string jumpLabel = "跳转到 " + percentText;
  M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, L.jump.x, L.jump.y, L.jump.width, L.jump.height,
                              jumpLabel.c_str(), true, EpdFontFamily::BOLD, 8);

  drawToolbar(renderer, pageWidth, pageHeight);

  firstPaint = false;
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
