#include "BookmarkManagerActivity.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"
#include "util/M4ListTouchPolicy.h"

namespace {
constexpr int LONG_PRESS_MS = 700;
}

int BookmarkManagerActivity::getPageItems() const {
  constexpr int lineHeight = 30;
  const int screenHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int startY = 60 + hintGutterHeight;
  const int availableHeight = screenHeight - startY - lineHeight;
  return std::max(1, availableHeight / lineHeight);
}

void BookmarkManagerActivity::taskTrampoline(void* param) {
  auto* self = static_cast<BookmarkManagerActivity*>(param);
  self->displayTaskLoop();
}

void BookmarkManagerActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();
  selectorIndex = 0;
  updateRequired = true;
  xTaskCreate(&BookmarkManagerActivity::taskTrampoline, "BookmarkMgrTask", 4096, this, 1, &displayTaskHandle);
}

void BookmarkManagerActivity::onExit() {
  ActivityWithSubactivity::onExit();
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void BookmarkManagerActivity::displayTaskLoop() {
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

void BookmarkManagerActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  const int totalItems = static_cast<int>(bookmarks.size());

  if (deleteConfirmMode) {
    // Touch: Yes/No dialog (geometry matches typical centered two buttons)
    if (mappedInput.hasTouch()) {
      if (mappedInput.wasBackGesture()) {
        deleteConfirmMode = false;
        deleteConfirmSelected = false;
        updateRequired = true;
        return;
      }
      int tx = 0, ty = 0;
      if (mappedInput.wasScreenTapped(tx, ty)) {
        // Split screen halves as Yes / No
        const int mid = renderer.getScreenWidth() / 2;
        deleteConfirmSelected = (tx < mid);
        if (deleteConfirmSelected) {
          onDeleteBookmark(selectorIndex);
          if (selectorIndex >= 0 && selectorIndex < totalItems) {
            bookmarks.erase(bookmarks.begin() + selectorIndex);
          }
          if (selectorIndex >= static_cast<int>(bookmarks.size()) && !bookmarks.empty()) {
            selectorIndex = static_cast<int>(bookmarks.size()) - 1;
          }
          if (bookmarks.empty()) selectorIndex = 0;
        }
        deleteConfirmMode = false;
        deleteConfirmSelected = false;
        updateRequired = true;
        return;
      }
    }
    // In delete confirmation mode
    if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      deleteConfirmSelected = !deleteConfirmSelected;
      updateRequired = true;
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
               mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      deleteConfirmSelected = !deleteConfirmSelected;
      updateRequired = true;
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (deleteConfirmSelected) {
        // Delete confirmed
        onDeleteBookmark(selectorIndex);
        // Remove from local list
        if (selectorIndex >= 0 && selectorIndex < totalItems) {
          bookmarks.erase(bookmarks.begin() + selectorIndex);
        }
        // Adjust selector
        if (selectorIndex >= static_cast<int>(bookmarks.size()) && !bookmarks.empty()) {
          selectorIndex = static_cast<int>(bookmarks.size()) - 1;
        }
        if (bookmarks.empty()) {
          selectorIndex = 0;
        }
      }
      deleteConfirmMode = false;
      deleteConfirmSelected = false;
      updateRequired = true;
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      deleteConfirmMode = false;
      deleteConfirmSelected = false;
      updateRequired = true;
    }
    return;
  }

  // Normal navigation mode — touch list (startY=60+contentY, lineH=30)
  if (mappedInput.hasTouch()) {
    if (mappedInput.wasBackGesture() || mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoBack();
      return;
    }
    if (totalItems > 0) {
      const auto orientation = renderer.getOrientation();
      const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
      const int contentY = isPortraitInverted ? 50 : 0;
      constexpr int lineHeight = 30;
      const int startY = 60 + contentY;
      const int pageHeight = renderer.getScreenHeight();
      constexpr int buttonHintsHeight = 40;
      const int availableHeight = pageHeight - startY - buttonHintsHeight;
      const int itemsPerPage = std::max(1, availableHeight / lineHeight);
      M4ListTouchPolicy::Event te{};
      te.backGesture = false;
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
        onJumpToBookmark(bookmarks[selectorIndex].percentage);
        return;
      }
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoBack();
    return;
  }

  if (totalItems == 0) {
    return;
  }

  // Long press confirm = delete
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= LONG_PRESS_MS) {
    deleteConfirmMode = true;
    deleteConfirmSelected = true;  // Default to "是"
    updateRequired = true;
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (mappedInput.getHeldTime() < LONG_PRESS_MS) {
      onJumpToBookmark(bookmarks[selectorIndex].percentage);
    }
    return;
  }

  const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);

  if (prevReleased) {
    selectorIndex = (selectorIndex + totalItems - 1) % totalItems;
    updateRequired = true;
  } else if (nextReleased) {
    selectorIndex = (selectorIndex + 1) % totalItems;
    updateRequired = true;
  }
}

void BookmarkManagerActivity::renderScreen() {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int contentY = hintGutterHeight;

  // Title
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, 15 + contentY, "书签管理", EpdFontFamily::BOLD);

  const int totalItems = static_cast<int>(bookmarks.size());
  const int startY = 60 + contentY;
  constexpr int lineHeight = 30;
  const int pageHeight = renderer.getScreenHeight();
  constexpr int buttonHintsHeight = 40;
  const int availableHeight = pageHeight - startY - buttonHintsHeight;
  const int itemsPerPage = std::max(1, availableHeight / lineHeight);

  if (totalItems == 0) {
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, startY + 30, "暂无书签");
  } else {
    // Calculate scroll offset
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
      const bool isSelected = (idx == selectorIndex);

      if (isSelected && !deleteConfirmMode) {
        renderer.fillRect(contentX, displayY, contentWidth - 1, lineHeight, true);
      }

      if (deleteConfirmMode && idx == selectorIndex) {
        // Draw delete confirmation inline
        const char* prompt = "删除?";
        M4UiText::draw(renderer, UI_10_FONT_ID, contentX + 10, displayY, prompt, true);

        const int btnY = displayY;
        const int yesX = contentX + 80;
        const int cancelX = contentX + 150;

        if (deleteConfirmSelected) {
          renderer.fillRect(yesX - 4, btnY, 40, lineHeight, true);
          M4UiText::draw(renderer, UI_10_FONT_ID, yesX, btnY, "[是]", false);
          M4UiText::draw(renderer, UI_10_FONT_ID, cancelX, btnY, "[取消]", true);
        } else {
          M4UiText::draw(renderer, UI_10_FONT_ID, yesX, btnY, "[是]", true);
          renderer.fillRect(cancelX - 4, btnY, 56, lineHeight, true);
          M4UiText::draw(renderer, UI_10_FONT_ID, cancelX, btnY, "[取消]", false);
        }
      } else {
        // Truncate title to fit
        const std::string truncTitle =
            M4UiText::truncated(renderer, UI_10_FONT_ID, bookmarks[idx].title.c_str(), contentWidth - 40);
        M4UiText::draw(renderer, UI_10_FONT_ID, contentX + 10, displayY, truncTitle.c_str(), !isSelected);
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

  const auto labels = mappedInput.mapLabels("« 返回", "选择/长按删除", "向上", "向下");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
