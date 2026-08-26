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
constexpr int SKIP_PAGE_MS = 700;

M4TouchListMetrics::ChapterListLayout chapterLayout(const GfxRenderer& renderer, bool touch) {
  const int layoutFont = touch ? UI_12_FONT_ID : UI_10_FONT_ID;
  return M4TouchListMetrics::makeChapterListLayout(
      renderer.getScreenWidth(), renderer.getScreenHeight(), touch,
      static_cast<TouchHitGeometry::Orientation>(renderer.getOrientation()),
      M4UiText::systemListLineHeight(renderer, layoutFont));
}
}  // namespace

int EpubReaderChapterSelectionActivity::getTotalItems() const { return epub->getTocItemsCount(); }

int EpubReaderChapterSelectionActivity::getPageItems() const {
  const bool touch = mappedInput.hasTouch();
  const auto layout = chapterLayout(renderer, touch);
  return std::max(1, layout.list.height / layout.rowHeight);
}

void EpubReaderChapterSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<EpubReaderChapterSelectionActivity*>(param);
  self->displayTaskLoop();
}

void EpubReaderChapterSelectionActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  M4TouchNavigation::activateForChapterSelection();
  if (!epub) return;

  EpdFontLoader::ensureFontsFromSd(renderer);
  renderingMutex = xSemaphoreCreateMutex();

  selectorIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (selectorIndex == -1) selectorIndex = 0;

  updateRequired = true;
  xTaskCreate(&EpubReaderChapterSelectionActivity::taskTrampoline, "EpubReaderChapterSelectionActivityTask",
              4096, this, 1, &displayTaskHandle);
}

void EpubReaderChapterSelectionActivity::onExit() {
  ActivityWithSubactivity::onExit();
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

  if (mappedInput.hasTouch() && totalItems > 0) {
    const auto chapterFrame = chapterLayout(renderer, true);
    const int lineHeight = chapterFrame.rowHeight;
    const int listTop = chapterFrame.list.y;
    const int listH = chapterFrame.list.height;

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
    if (newSpineIndex == -1) onGoBack();
    else onSelectSpineIndex(newSpineIndex);
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoBack();
  } else if (prevReleased) {
    const bool isUpKey = mappedInput.wasReleased(MappedInputManager::Button::Up);
    if (skipPage || isUpKey) {
      selectorIndex = ((selectorIndex / pageItems - 1) * pageItems + totalItems) % totalItems;
    } else {
      selectorIndex = (selectorIndex + totalItems - 1) % totalItems;
    }
    updateRequired = true;
  } else if (nextReleased) {
    const bool isDownKey = mappedInput.wasReleased(MappedInputManager::Button::Down);
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
  // TOC entry follows either the reader/menu or reader-settings path. Start a
  // complete logical frame so no parent status-bar pixels can be retained.
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const int pageItems = getPageItems();
  const int totalItems = getTotalItems();

  const bool touch = mappedInput.hasTouch();
  const auto layout = chapterLayout(renderer, touch);
  const int contentX = layout.list.x;
  const int contentWidth = layout.list.width;
  const int lineHeight = layout.rowHeight;
  const int listTop = layout.list.y;
  const int layoutFont = touch ? UI_12_FONT_ID : UI_10_FONT_ID;
  (void)M4UiText::resolveSystem(renderer, layoutFont);

  GUI.drawHeader(renderer, Rect{layout.header.x, layout.header.y, layout.header.width, layout.header.height}, "目录");

  const int pageStartIndex = selectorIndex / pageItems * pageItems;
  const int currentTocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);

  for (int i = 0; i < pageItems; ++i) {
    const int itemIndex = pageStartIndex + i;
    if (itemIndex >= totalItems) break;

    const int rowY = listTop + i * lineHeight;
    const int displayY = rowY + (touch ? 10 : 0);
    const bool isSelected = itemIndex == selectorIndex;
    const bool isCurrent = itemIndex == currentTocIndex;

    // E-ink hierarchy: the currently focused row is an outline, while the
    // actual reading chapter gets a narrow ink marker. Avoiding whole-row
    // inversion greatly reduces changed pixels during navigation.
    if (isSelected) {
      renderer.drawRect(contentX + 8, rowY + 2, std::max(1, contentWidth - 17), std::max(1, lineHeight - 4), true);
    }
    if (isCurrent) {
      renderer.fillRect(contentX + 12, rowY + 10, 4, std::max(8, lineHeight - 20), true);
    }

    const auto item = epub->getTocItem(itemIndex);
    const int indentSize = contentX + 24 + std::max(0, item.level - 1) * 15;
    const int availableTextWidth = std::max(24, contentX + contentWidth - 20 - indentSize);
    const auto weight = (isSelected || isCurrent) ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const std::string chapterName =
        M4UiText::truncatedSystem(renderer, layoutFont, item.title.c_str(), availableTextWidth, weight);
    M4UiText::drawSystem(renderer, layoutFont, indentSize, displayY, chapterName.c_str(), true, weight);
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
      renderer.drawRect(x, pagerTop, pagerWidth, pagerHeight, true);
      if (enabled && pagerWidth > 6 && pagerHeight > 6) {
        renderer.drawRect(x + 2, pagerTop + 2, pagerWidth - 4, pagerHeight - 4, true);
      }
      M4UiText::drawCenteredInBoxSystem(renderer, UI_10_FONT_ID, x, pagerTop, pagerWidth, pagerHeight,
                                         label, true, enabled ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR, 8);
    };
    drawPagerButton(leftX, "上一页", currentPage > 1);
    drawPagerButton(rightX, "下一页", currentPage < totalPages);
    M4UiText::drawCenteredInBoxSystem(renderer, UI_10_FONT_ID, 0,
                                      M4TouchListMetrics::chapterPagerLabelTop(renderer.getScreenHeight(), true), pageWidth,
                                      M4TouchListMetrics::chapterPagerLabelHeight(true), pageLabel, true,
                                      EpdFontFamily::REGULAR, 8);
  } else {
    const auto labels = mappedInput.mapLabels("« 返回", "选择", "向上", "向下");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  // displayBuffer submits the complete framebuffer; FAST is sufficient because
  // the frame above explicitly redraws the entire header/list/footer.
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
