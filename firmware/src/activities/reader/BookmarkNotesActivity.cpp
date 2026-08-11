#include "BookmarkNotesActivity.h"

#include <GfxRenderer.h>

#include <map>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"
#include "util/M4ListTouchPolicy.h"

int BookmarkNotesActivity::getPageItems() const {
  constexpr int lineHeight = 30;
  const int screenHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int startY = 60 + hintGutterHeight;
  const int availableHeight = screenHeight - startY - lineHeight;
  return std::max(1, availableHeight / lineHeight);
}

void BookmarkNotesActivity::buildItems() {
  items.clear();
  auto allBookmarks = BookmarkStore::loadAllBookmarks();

  // Group by bookPath, preserving order (already sorted by timestamp desc)
  std::vector<std::string> bookOrder;
  std::map<std::string, std::vector<Bookmark>> grouped;

  for (auto& bm : allBookmarks) {
    if (grouped.find(bm.bookPath) == grouped.end()) {
      bookOrder.push_back(bm.bookPath);
    }
    grouped[bm.bookPath].push_back(std::move(bm));
  }

  for (const auto& bookPath : bookOrder) {
    const auto& bookmarks = grouped[bookPath];
    if (bookmarks.empty()) continue;

    // Add separator with book title
    BookmarkNoteItem sep;
    sep.isSeparator = true;
    sep.displayText = bookmarks[0].bookTitle.empty() ? bookPath : bookmarks[0].bookTitle;
    sep.bookPath = bookPath;
    sep.percentage = 0;
    items.push_back(std::move(sep));

    // Add bookmark items
    for (const auto& bm : bookmarks) {
      BookmarkNoteItem item;
      item.isSeparator = false;
      item.displayText = bm.title;
      item.bookPath = bm.bookPath;
      item.percentage = bm.percentage;
      items.push_back(std::move(item));
    }
  }
}

void BookmarkNotesActivity::moveToNextSelectable(int direction) {
  if (items.empty()) return;

  const int total = static_cast<int>(items.size());
  int next = selectorIndex;

  // Try to find next selectable item (non-separator)
  for (int i = 0; i < total; i++) {
    next = (next + direction + total) % total;
    if (!items[next].isSeparator) {
      selectorIndex = next;
      return;
    }
  }
  // All items are separators (shouldn't happen), stay put
}

void BookmarkNotesActivity::taskTrampoline(void* param) {
  auto* self = static_cast<BookmarkNotesActivity*>(param);
  self->displayTaskLoop();
}

void BookmarkNotesActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();

  buildItems();

  // Find first selectable item
  selectorIndex = 0;
  if (!items.empty() && items[0].isSeparator) {
    moveToNextSelectable(1);
  }

  updateRequired = true;
  xTaskCreate(&BookmarkNotesActivity::taskTrampoline, "BookmarkNotesTask", 4096, this, 1, &displayTaskHandle);
}

void BookmarkNotesActivity::onExit() {
  ActivityWithSubactivity::onExit();
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void BookmarkNotesActivity::displayTaskLoop() {
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

void BookmarkNotesActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      (mappedInput.hasTouch() && mappedInput.wasBackGesture())) {
    onGoBack();
    return;
  }

  if (items.empty()) return;

  // Check if any selectable items exist
  bool hasSelectable = false;
  for (const auto& item : items) {
    if (!item.isSeparator) {
      hasSelectable = true;
      break;
    }
  }
  if (!hasSelectable) return;

  // Touch list: startY=60, lineH=30 (matches renderScreen)
  if (mappedInput.hasTouch()) {
    constexpr int startY = 60;
    constexpr int lineHeight = 30;
    constexpr int buttonHintsHeight = 40;
    const int pageHeight = renderer.getScreenHeight();
    const int itemsPerPage = std::max(1, (pageHeight - startY - buttonHintsHeight) / lineHeight);
    const int totalItems = static_cast<int>(items.size());
    M4ListTouchPolicy::Event te{};
    const auto sw = mappedInput.wasSwipe();
    if (sw == MappedInputManager::SwipeDir::Up) te.swipe = M4ListTouchPolicy::Swipe::Up;
    else if (sw == MappedInputManager::SwipeDir::Down) te.swipe = M4ListTouchPolicy::Swipe::Down;
    int dx = 0, dy = 0, tx = 0, ty = 0;
    te = M4ListTouchPolicy::mergeFrame(false, te.swipe, mappedInput.wasScreenTouchDown(dx, dy), dx, dy,
                                       mappedInput.wasScreenTapped(tx, ty), tx, ty);
    M4ListTouchPolicy::ListLayout layout;
    layout.listTop = startY;
    layout.listHeight = itemsPerPage * lineHeight;
    layout.rowStep = lineHeight;
    layout.itemCount = totalItems;
    layout.selectedIndex = selectorIndex;
    int hit = -1;
    const auto act = M4ListTouchPolicy::resolveList(te, layout, hit);
    if (act == M4ListTouchPolicy::Action::PageDown || act == M4ListTouchPolicy::Action::PageUp) {
      selectorIndex = M4ListTouchPolicy::applyPage(selectorIndex, totalItems, itemsPerPage,
                                                   act == M4ListTouchPolicy::Action::PageDown);
      if (selectorIndex >= 0 && selectorIndex < totalItems && items[selectorIndex].isSeparator) {
        moveToNextSelectable(act == M4ListTouchPolicy::Action::PageDown ? 1 : -1);
      }
      updateRequired = true;
      return;
    }
    if (act == M4ListTouchPolicy::Action::Select && hit >= 0) {
      if (!items[hit].isSeparator && selectorIndex != hit) {
        selectorIndex = hit;
        updateRequired = true;
      }
      return;
    }
    if (act == M4ListTouchPolicy::Action::Activate && hit >= 0 && !items[hit].isSeparator) {
      selectorIndex = hit;
      onOpenBookAtBookmark(items[selectorIndex].bookPath, items[selectorIndex].percentage);
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectorIndex >= 0 && selectorIndex < static_cast<int>(items.size()) && !items[selectorIndex].isSeparator) {
      onOpenBookAtBookmark(items[selectorIndex].bookPath, items[selectorIndex].percentage);
    }
    return;
  }

  const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);

  if (prevReleased) {
    moveToNextSelectable(-1);
    updateRequired = true;
  } else if (nextReleased) {
    moveToNextSelectable(1);
    updateRequired = true;
  }
}

void BookmarkNotesActivity::renderScreen() {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Title
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, 15, "书签笔记", EpdFontFamily::BOLD);

  const int totalItems = static_cast<int>(items.size());
  const int startY = 60;
  constexpr int lineHeight = 30;
  constexpr int buttonHintsHeight = 40;
  const int availableHeight = pageHeight - startY - buttonHintsHeight;
  const int itemsPerPage = std::max(1, availableHeight / lineHeight);

  if (totalItems == 0) {
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, startY + 30, "暂无书签");
  } else {
    // Calculate scroll offset to keep selector visible
    int scrollOffset = 0;
    if (selectorIndex >= itemsPerPage) {
      scrollOffset = selectorIndex - itemsPerPage + 1;
    }
    if (scrollOffset > totalItems - itemsPerPage) {
      scrollOffset = std::max(0, totalItems - itemsPerPage);
    }

    const int visibleEnd = std::min(scrollOffset + itemsPerPage, totalItems);

    for (int idx = scrollOffset; idx < visibleEnd; ++idx) {
      const int displayY = startY + (idx - scrollOffset) * lineHeight;
      const bool isSelected = (idx == selectorIndex && !items[idx].isSeparator);

      if (items[idx].isSeparator) {
        // Draw separator: book title in bold with a line underneath
        const std::string truncTitle =
            M4UiText::truncated(renderer, UI_10_FONT_ID, items[idx].displayText.c_str(), pageWidth - 30, EpdFontFamily::BOLD);
        M4UiText::draw(renderer, UI_10_FONT_ID, 10, displayY, truncTitle.c_str(), true, EpdFontFamily::BOLD);
        renderer.drawLine(10, displayY + lineHeight - 2, pageWidth - 10, displayY + lineHeight - 2, true);
      } else {
        // Draw bookmark item
        if (isSelected) {
          renderer.fillRect(0, displayY, pageWidth - 1, lineHeight, true);
        }
        const std::string truncTitle =
            M4UiText::truncated(renderer, UI_10_FONT_ID, items[idx].displayText.c_str(), pageWidth - 40);
        M4UiText::draw(renderer, UI_10_FONT_ID, 20, displayY, truncTitle.c_str(), !isSelected);
      }
    }

    // Scrollbar
    if (totalItems > itemsPerPage) {
      auto metrics = UITheme::getInstance().getMetrics();
      const int scrollBarX = pageWidth - metrics.scrollBarRightOffset;
      const int scrollBarHeight = std::max(20, (availableHeight * itemsPerPage) / totalItems);
      const int scrollBarY =
          startY + ((availableHeight - scrollBarHeight) * scrollOffset) / std::max(1, totalItems - itemsPerPage);
      renderer.drawLine(scrollBarX, startY, scrollBarX, startY + availableHeight, true);
      renderer.fillRect(scrollBarX - metrics.scrollBarWidth, scrollBarY, metrics.scrollBarWidth, scrollBarHeight, true);
    }
  }

  const auto labels = mappedInput.mapLabels("« 返回", "选择", "向上", "向下");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
