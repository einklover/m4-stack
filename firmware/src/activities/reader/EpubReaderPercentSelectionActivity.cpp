#include "EpubReaderPercentSelectionActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>

#include <algorithm>
#include <string>

#include "MappedInputManager.h"
#include "fontIds.h"
#include "util/M4UiText.h"
#include "util/TouchHitGeometry.h"

namespace {
constexpr int kSmallStep = 1;
constexpr int kLargeStep = 10;
constexpr int kPanelHeight = 270;
constexpr int kStepCount = 4;
constexpr int kStepDeltas[kStepCount] = {-10, -1, 1, 10};
constexpr const char* kStepLabels[kStepCount] = {"-10%", "-1%", "+1%", "+10%"};

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
  const int top = std::max(72, height - kPanelHeight);
  L.panel = {0, top, width, height - top};
  L.close = {width - 64, top, 64, 52};
  L.value = {24, top + 38, std::max(80, width - 48), 48};
  L.track = {24, top + 94, std::max(80, width - 48), 10};
  L.trackHit = {24, top + 78, std::max(80, width - 48), 42};

  constexpr int gap = 8;
  const int innerW = std::max(120, width - 48);
  const int stepW = std::max(48, (innerW - gap * (kStepCount - 1)) / kStepCount);
  const int usedW = stepW * kStepCount + gap * (kStepCount - 1);
  const int startX = 24 + std::max(0, (innerW - usedW) / 2);
  const int stepY = top + 134;
  for (int i = 0; i < kStepCount; ++i) {
    L.steps[i] = {startX + i * (stepW + gap), stepY, stepW, 50};
  }

  const int jumpW = std::min(180, std::max(120, width / 3));
  L.jump = {(width - jumpW) / 2, top + 200, jumpW, 52};
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
    subActivity->loop();
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
      const auto L = makeProgressSheet(renderer.getScreenWidth(), renderer.getScreenHeight());

      // Exposed text remains a real dismissal target; no hidden full-screen page.
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
  // Preserve the reader framebuffer and replace only the bottom sheet region.
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

  firstPaint = false;
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
