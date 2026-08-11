#include "EpubReaderChapterSelectionActivity.h"

#include <EpdFontLoader.h>
#include <GfxRenderer.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4ListTouchPolicy.h"
#include "util/M4TouchListMetrics.h"
#include "util/M4UiText.h"

namespace {
// Time threshold for treating a long press as a page-up/page-down
constexpr int SKIP_PAGE_MS = 700;
}  // namespace

int EpubReaderChapterSelectionActivity::getTotalItems() const { return epub->getTocItemsCount(); }

int EpubReaderChapterSelectionActivity::getPageItems() const {
  const bool touch = mappedInput.hasTouch();
  const int lineHeight = M4TouchListMetrics::chapterLineHeight(touch);

  const int screenHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();
  // In inverted portrait, the button hints are drawn near the logical top.
  // Reserve vertical space so list items do not collide with the hints.
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int startY = M4TouchListMetrics::chapterListTop(touch) + hintGutterHeight;
  const int footerReserve = touch ? M4TouchListMetrics::chapterFooterReserve(true) : lineHeight;
  const int availableHeight = screenHeight - startY - footerReserve;
  // Clamp to at least one item to avoid division by zero and empty paging.
  return std::max(1, availableHeight / lineHeight);
}

void EpubReaderChapterSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<EpubReaderChapterSelectionActivity*>(param);
  self->displayTaskLoop();
}

void EpubReaderChapterSelectionActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  if (!epub) {
    return;
  }

  // Full-CJK TOC titles; skip rescan if already loaded this session.
  EpdFontLoader::ensureFontsFromSd(renderer);

  renderingMutex = xSemaphoreCreateMutex();

  selectorIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (selectorIndex == -1) {
    selectorIndex = 0;
  }

  // Trigger first update
  updateRequired = true;
  xTaskCreate(&EpubReaderChapterSelectionActivity::taskTrampoline, "EpubReaderChapterSelectionActivityTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void EpubReaderChapterSelectionActivity::onExit() {
  ActivityWithSubactivity::onExit();

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void EpubReaderChapterSelectionActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);

  const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;
  const int pageItems = getPageItems();
  const int totalItems = getTotalItems();

  // Touch: same geometry as renderScreen() (touch-optimized row height)
  if (mappedInput.hasTouch() && totalItems > 0) {
    const auto orientation = renderer.getOrientation();
    const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
    const int contentY = isPortraitInverted ? 50 : 0;
    const int lineHeight = M4TouchListMetrics::chapterLineHeight(true);
    const int listTop = M4TouchListMetrics::chapterListTop(true) + contentY;
    const int listH = pageItems * lineHeight;

    // The touch footer is a real part of the chapter-list contract.  Handle
    // it before the generic list policy so a tap cannot be interpreted as a
    // row hit or silently ignored below the list.
    const int pagerTop = M4TouchListMetrics::chapterPagerTop(renderer.getScreenHeight(), true);
    const int pagerHeight = M4TouchListMetrics::chapterPagerButtonHeight(true);
    const int pagerWidth = M4TouchListMetrics::chapterPagerButtonWidth(renderer.getScreenWidth(), true);
    const int pagerLeft = M4TouchListMetrics::chapterPagerLeftX(true);
    const int pagerRight = M4TouchListMetrics::chapterPagerRightX(renderer.getScreenWidth(), true);
    int downX = 0, downY = 0, tapX = 0, tapY = 0;
    const bool touchDown = mappedInput.wasScreenTouchDown(downX, downY);
    const bool tapped = mappedInput.wasScreenTapped(tapX, tapY);
    const int eventX = tapped ? tapX : downX;
    const int eventY = tapped ? tapY : downY;
    if ((touchDown || tapped) && eventY >= pagerTop && eventY < pagerTop + pagerHeight) {
      if (tapped && eventX >= pagerLeft && eventX < pagerLeft + pagerWidth) {
        if (selectorIndex >= pageItems) {
          selectorIndex -= pageItems;
          updateRequired = true;
        }
      } else if (tapped && eventX >= pagerRight && eventX < pagerRight + pagerWidth) {
        if (selectorIndex + pageItems < totalItems) {
          selectorIndex += pageItems;
          updateRequired = true;
        }
      }
      return;
    }

    M4ListTouchPolicy::Event te{};
    te.backGesture = mappedInput.wasBackGesture();
    const auto sw = mappedInput.wasSwipe();
    if (sw == MappedInputManager::SwipeDir::Up) te.swipe = M4ListTouchPolicy::Swipe::Up;
    else if (sw == MappedInputManager::SwipeDir::Down) te.swipe = M4ListTouchPolicy::Swipe::Down;
    else if (sw == MappedInputManager::SwipeDir::Left) te.swipe = M4ListTouchPolicy::Swipe::Left;
    else if (sw == MappedInputManager::SwipeDir::Right) te.swipe = M4ListTouchPolicy::Swipe::Right;
    int dx = 0, dy = 0, tx = 0, ty = 0;
    te = M4ListTouchPolicy::mergeFrame(te.backGesture, te.swipe, mappedInput.wasScreenTouchDown(dx, dy), dx, dy,
                                       mappedInput.wasScreenTapped(tx, ty), tx, ty);
    M4ListTouchPolicy::ListLayout layout;
    layout.listTop = listTop;
    layout.listHeight = listH;
    layout.rowStep = lineHeight;
    layout.itemCount = totalItems;
    layout.selectedIndex = selectorIndex;
    layout.maxVisible = 0;
    int hit = -1;
    const auto act = M4ListTouchPolicy::resolveList(te, layout, hit);
    if (act == M4ListTouchPolicy::Action::Back) {
      onGoBack();
      return;
    }
    if (act == M4ListTouchPolicy::Action::PageDown || act == M4ListTouchPolicy::Action::PageUp) {
      selectorIndex = M4ListTouchPolicy::applyPage(selectorIndex, totalItems, pageItems,
                                                   act == M4ListTouchPolicy::Action::PageDown);
      updateRequired = true;
      return;
    }
    if (act == M4ListTouchPolicy::Action::Select && hit >= 0) {
      if (selectorIndex != hit) {
        selectorIndex = hit;
        updateRequired = true;
      }
      return;
    }
    if (act == M4ListTouchPolicy::Action::Activate && hit >= 0) {
      selectorIndex = hit;
      const auto newSpineIndex = epub->getSpineIndexForTocIndex(selectorIndex);
      if (newSpineIndex == -1) onGoBack();
      else onSelectSpineIndex(newSpineIndex);
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto newSpineIndex = epub->getSpineIndexForTocIndex(selectorIndex);
    if (newSpineIndex == -1) {
      onGoBack();
    } else {
      onSelectSpineIndex(newSpineIndex);
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoBack();
  } else if (prevReleased) {
    bool isUpKey = mappedInput.wasReleased(MappedInputManager::Button::Up);
    if (skipPage || isUpKey) {
      selectorIndex = ((selectorIndex / pageItems - 1) * pageItems + totalItems) % totalItems;
    } else {
      selectorIndex = (selectorIndex + totalItems - 1) % totalItems;
    }
    updateRequired = true;
  } else if (nextReleased) {
    bool isDownKey = mappedInput.wasReleased(MappedInputManager::Button::Down);
    if (skipPage || isDownKey) {
      selectorIndex = ((selectorIndex / pageItems + 1) * pageItems) % totalItems;
    } else {
      selectorIndex = (selectorIndex + 1) % totalItems;
    }
    updateRequired = true;
  }
}

void EpubReaderChapterSelectionActivity::displayTaskLoop() {
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

void EpubReaderChapterSelectionActivity::renderScreen() {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto orientation = renderer.getOrientation();
  // Landscape orientation: reserve a horizontal gutter for button hints.
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  // Inverted portrait: reserve vertical space for hints at the top.
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  // Landscape CW places hints on the left edge; CCW keeps them on the right.
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int contentY = hintGutterHeight;
  const int pageItems = getPageItems();
  const int totalItems = getTotalItems();

  const bool touch = mappedInput.hasTouch();
  const int lineHeight = M4TouchListMetrics::chapterLineHeight(touch);
  const int listTop = M4TouchListMetrics::chapterListTop(touch) + contentY;
  const int titleY = M4TouchListMetrics::chapterTitleY(touch) + contentY;
  // Shared UI-text path (reader face scaled to chrome metrics).
  const int layoutFont = touch ? UI_12_FONT_ID : UI_10_FONT_ID;
  const auto rowFace = M4UiText::resolveChapterRow(renderer, layoutFont);
  const int rowFont = rowFace.fontId;
  const float rowScale = rowFace.scale;

  // Manual centering to honor content gutters.
  const int titleW = M4UiText::textWidth(renderer, UI_12_FONT_ID, "目录", EpdFontFamily::BOLD);
  const int titleX = contentX + (contentWidth - titleW) / 2;
  M4UiText::draw(renderer, UI_12_FONT_ID, titleX, titleY, "目录", true, EpdFontFamily::BOLD);

  const auto pageStartIndex = selectorIndex / pageItems * pageItems;
  // Highlight only the content area, not the hint gutters.
  renderer.fillRect(contentX, listTop + (selectorIndex % pageItems) * lineHeight - 2, contentWidth - 1,
                    lineHeight);

  for (int i = 0; i < pageItems; i++) {
    int itemIndex = pageStartIndex + i;
    if (itemIndex >= totalItems) break;
    const int displayY = listTop + i * lineHeight + (touch ? 10 : 0);
    const bool isSelected = (itemIndex == selectorIndex);

    auto item = epub->getTocItem(itemIndex);

    // Indent per TOC level while keeping content within the gutter-safe region.
    const int indentSize = contentX + 20 + (item.level - 1) * 15;
    const std::string chapterName = renderer.truncatedText(rowFont, item.title.c_str(),
                                                          contentWidth - 40 - indentSize, EpdFontFamily::REGULAR,
                                                          rowScale);

    renderer.drawText(rowFont, indentSize, displayY, chapterName.c_str(), !isSelected, EpdFontFamily::REGULAR,
                      rowScale);
  }

  if (touch) {
    const int pagerTop = M4TouchListMetrics::chapterPagerTop(renderer.getScreenHeight(), true);
    const int pagerHeight = M4TouchListMetrics::chapterPagerButtonHeight(true);
    const int pagerWidth = M4TouchListMetrics::chapterPagerButtonWidth(pageWidth, true);
    const int leftX = M4TouchListMetrics::chapterPagerLeftX(true);
    const int rightX = M4TouchListMetrics::chapterPagerRightX(pageWidth, true);
    const int totalPages = std::max(1, (totalItems + pageItems - 1) / pageItems);
    const int currentPage = std::min(totalPages, selectorIndex / pageItems + 1);
    char pageLabel[48];
    snprintf(pageLabel, sizeof(pageLabel), "%d/%d", currentPage, totalPages);
    const auto drawPagerButton = [&](int x, const char* label, bool enabled) {
      renderer.fillRoundedRect(x, pagerTop, pagerWidth, pagerHeight, 8,
                               enabled ? Color::LightGray : Color::White);
      renderer.drawRoundedRect(x, pagerTop, pagerWidth, pagerHeight, 1, 8, true);
      M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, x, pagerTop, pagerWidth, pagerHeight, label, true);
    };
    drawPagerButton(leftX, "‹ 上一页", currentPage > 1);
    drawPagerButton(rightX, "下一页 ›", currentPage < totalPages);
    M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, 0,
                                M4TouchListMetrics::chapterPagerLabelTop(renderer.getScreenHeight(), true), pageWidth,
                                M4TouchListMetrics::chapterPagerLabelHeight(true), pageLabel, true,
                                EpdFontFamily::REGULAR, 8);
  } else {
    const auto labels = mappedInput.mapLabels("« 返回", "选择", "向上", "向下");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
