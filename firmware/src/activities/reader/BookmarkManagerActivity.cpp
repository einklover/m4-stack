#include "BookmarkManagerActivity.h"

#include <EpdFontLoader.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4ListTouchPolicy.h"
#include "util/M4UiText.h"
#include "util/TouchHitGeometry.h"

namespace {
constexpr int LONG_PRESS_MS = 700;
constexpr int kTouchRowHeight = 56;
constexpr int kDeleteZoneWidth = 56;
constexpr int kRowSide = 8;

struct DeleteDialogLayout {
  TouchHitGeometry::Rect box{};
  TouchHitGeometry::Rect yes{};
  TouchHitGeometry::Rect no{};
};

DeleteDialogLayout makeDeleteDialogLayout(int screenWidth, int screenHeight) {
  DeleteDialogLayout L;
  const int boxW = std::min(320, std::max(240, screenWidth - 48));
  constexpr int boxH = 142;
  constexpr int pad = 16;
  constexpr int gap = 12;
  constexpr int buttonH = 52;
  const int boxX = (screenWidth - boxW) / 2;
  const int boxY = (screenHeight - boxH) / 2;
  const int buttonW = (boxW - pad * 2 - gap) / 2;
  L.box = {boxX, boxY, boxW, boxH};
  L.yes = {boxX + pad, boxY + boxH - pad - buttonH, buttonW, buttonH};
  L.no = {boxX + pad + buttonW + gap, boxY + boxH - pad - buttonH, buttonW, buttonH};
  return L;
}
}  // namespace

int BookmarkManagerActivity::getPageItems() const {
  const auto metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = mappedInput.hasTouch() ? kTouchRowHeight : 30;
  const int screenHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int startY = hintGutterHeight + metrics.headerHeight + metrics.verticalSpacing;
  const int availableHeight = screenHeight - startY - metrics.buttonHintsHeight - metrics.verticalSpacing;
  return std::max(1, availableHeight / lineHeight);
}

void BookmarkManagerActivity::taskTrampoline(void* param) {
  auto* self = static_cast<BookmarkManagerActivity*>(param);
  self->displayTaskLoop();
}

void BookmarkManagerActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  // Bookmark titles come from arbitrary book/chapter/body text. Keep fixed UI
  // chrome on the builtin 15x16 system font, but make sure the full reader
  // CJK face is available before rendering user content so uncommon
  // characters never '?'.
  EpdFontLoader::ensureFontsFromSd(renderer);
  renderingMutex = xSemaphoreCreateMutex();
  selectorIndex = 0;
  firstPaint_ = true;
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

  auto deleteSelectedBookmark = [this]() {
    const int total = static_cast<int>(bookmarks.size());
    if (selectorIndex < 0 || selectorIndex >= total) return;
    onDeleteBookmark(selectorIndex);
    bookmarks.erase(bookmarks.begin() + selectorIndex);
    if (selectorIndex >= static_cast<int>(bookmarks.size()) && !bookmarks.empty()) {
      selectorIndex = static_cast<int>(bookmarks.size()) - 1;
    }
    if (bookmarks.empty()) selectorIndex = 0;
  };

  if (deleteConfirmMode) {
    if (mappedInput.hasTouch()) {
      if (mappedInput.wasBackGesture()) {
        deleteConfirmMode = false;
        deleteConfirmSelected = false;
        updateRequired = true;
        return;
      }
      int tx = 0;
      int ty = 0;
      if (mappedInput.wasScreenTapped(tx, ty)) {
        const auto dialog = makeDeleteDialogLayout(renderer.getScreenWidth(), renderer.getScreenHeight());
        if (dialog.yes.contains(tx, ty)) {
          deleteSelectedBookmark();
          deleteConfirmMode = false;
          deleteConfirmSelected = false;
          updateRequired = true;
        } else if (dialog.no.contains(tx, ty)) {
          deleteConfirmMode = false;
          deleteConfirmSelected = false;
          updateRequired = true;
        }
        return;
      }
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right) ||
        mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      deleteConfirmSelected = !deleteConfirmSelected;
      updateRequired = true;
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (deleteConfirmSelected) deleteSelectedBookmark();
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

  const int totalItems = static_cast<int>(bookmarks.size());

  if (mappedInput.hasTouch()) {
    if (mappedInput.wasBackGesture() || mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoBack();
      return;
    }

    if (totalItems > 0) {
      const auto metrics = UITheme::getInstance().getMetrics();
      const auto orientation = renderer.getOrientation();
      const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
      const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
      const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
      const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
      const int contentX = isLandscapeCw ? hintGutterWidth : 0;
      const int contentWidth = renderer.getScreenWidth() - hintGutterWidth;
      const int contentY = isPortraitInverted ? 50 : 0;
      const int startY = contentY + metrics.headerHeight + metrics.verticalSpacing;
      const int itemsPerPage = getPageItems();
      const int pageStart = (selectorIndex / itemsPerPage) * itemsPerPage;
      const int listHeight = itemsPerPage * kTouchRowHeight;
      const int deleteX = contentX + contentWidth - kRowSide - kDeleteZoneWidth;

      const auto swipe = mappedInput.wasSwipe();
      int dx = 0, dy = 0, tx = 0, ty = 0;
      const bool touchDown = mappedInput.wasScreenTouchDown(dx, dy);
      const bool tapped = mappedInput.wasScreenTapped(tx, ty);

      // Explicit delete affordance: only the right-side delete cell opens the
      // confirmation dialog. A broad screen-half tap can never delete a bookmark.
      if (tapped && ty >= startY && ty < startY + listHeight && tx >= deleteX) {
        const int row = (ty - startY) / kTouchRowHeight;
        const int hit = pageStart + row;
        if (hit >= 0 && hit < totalItems) {
          selectorIndex = hit;
          deleteConfirmMode = true;
          deleteConfirmSelected = false;
          updateRequired = true;
        }
        return;
      }

      M4ListTouchPolicy::Event te{};
      if (swipe == MappedInputManager::SwipeDir::Up) te.swipe = M4ListTouchPolicy::Swipe::Up;
      else if (swipe == MappedInputManager::SwipeDir::Down) te.swipe = M4ListTouchPolicy::Swipe::Down;
      te = M4ListTouchPolicy::mergeFrame(false, te.swipe, touchDown, dx, dy, tapped, tx, ty);

      M4ListTouchPolicy::ListLayout layout;
      layout.listTop = startY;
      layout.listHeight = listHeight;
      layout.rowStep = kTouchRowHeight;
      layout.itemCount = totalItems;
      layout.selectedIndex = selectorIndex;
      layout.maxVisible = itemsPerPage;

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
  if (totalItems == 0) return;

  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= LONG_PRESS_MS) {
    deleteConfirmMode = true;
    deleteConfirmSelected = false;
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
  const int contentY = isPortraitInverted ? 50 : 0;

  GUI.drawHeader(renderer, Rect{contentX, contentY, contentWidth, metrics.headerHeight}, "书签");

  const int totalItems = static_cast<int>(bookmarks.size());
  const int lineHeight = mappedInput.hasTouch() ? kTouchRowHeight : 30;
  const int startY = contentY + metrics.headerHeight + metrics.verticalSpacing;
  const int itemsPerPage = getPageItems();
  const int availableHeight = itemsPerPage * lineHeight;

  if (totalItems == 0) {
    M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, contentX, startY + 24,
                                contentWidth, 48, "暂无书签", true, EpdFontFamily::REGULAR, 8);
  } else {
    const int pageStart = (selectorIndex / itemsPerPage) * itemsPerPage;
    const int visibleEnd = std::min(pageStart + itemsPerPage, totalItems);
    const int deleteX = contentX + contentWidth - kRowSide - kDeleteZoneWidth;
    const int percentWidth = 52;
    const int percentX = deleteX - percentWidth - 6;

    for (int idx = pageStart; idx < visibleEnd; ++idx) {
      const int row = idx - pageStart;
      const int rowY = startY + row * lineHeight;
      const bool selected = idx == selectorIndex;

      if (selected) {
        renderer.drawRect(contentX + kRowSide, rowY + 2,
                          std::max(1, contentWidth - kRowSide * 2), std::max(1, lineHeight - 4), true);
      }

      const int titleX = contentX + kRowSide + 10;
      const int titleWidth = std::max(30, percentX - titleX - 8);
      const auto weight = selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
      // User/book content is not UI chrome: use the full reader CJK face. This
      // preserves uncommon Han characters while percent/delete controls remain
      // on the small fixed UI face for crisp touch chrome.
      const std::string title = M4UiText::truncated(renderer, NOTOSANS_12_FONT_ID,
                                                    bookmarks[idx].title.c_str(), titleWidth, weight);
      M4UiText::draw(renderer, NOTOSANS_12_FONT_ID, titleX, rowY + 16, title.c_str(), true, weight);

      const int percentage = std::max(0, std::min(100,
          static_cast<int>(bookmarks[idx].percentage * 100.0f + 0.5f)));
      char percentageText[12];
      snprintf(percentageText, sizeof(percentageText), "%d%%", percentage);
      M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, percentX, rowY + 4,
                                  percentWidth, lineHeight - 8, percentageText, true,
                                  selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR, 6);

      renderer.fillRect(deleteX, rowY + 10, 1, std::max(8, lineHeight - 20), true);
      M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, deleteX, rowY + 4,
                                  kDeleteZoneWidth, lineHeight - 8, "删", true,
                                  EpdFontFamily::REGULAR, 6);
    }

    if (totalItems > itemsPerPage) {
      const int scrollBarX = contentX + contentWidth - metrics.scrollBarRightOffset;
      const int scrollBarHeight = std::max(20, (availableHeight * itemsPerPage) / totalItems);
      const int maxPageStart = std::max(1, ((totalItems - 1) / itemsPerPage) * itemsPerPage);
      const int scrollBarY = startY +
          ((availableHeight - scrollBarHeight) * pageStart) / maxPageStart;
      renderer.drawLine(scrollBarX, startY, scrollBarX, startY + availableHeight, true);
      renderer.fillRect(scrollBarX - metrics.scrollBarWidth, scrollBarY,
                        metrics.scrollBarWidth, scrollBarHeight, true);
    }
  }

  if (deleteConfirmMode && !bookmarks.empty()) {
    const auto dialog = makeDeleteDialogLayout(pageWidth, pageHeight);
    renderer.fillRect(dialog.box.x, dialog.box.y, dialog.box.width, dialog.box.height, false);
    renderer.drawRect(dialog.box.x, dialog.box.y, dialog.box.width, dialog.box.height, true);
    M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, dialog.box.x, dialog.box.y + 12,
                                dialog.box.width, 36, "删除这个书签?", true,
                                EpdFontFamily::BOLD, 8);

    const auto drawDialogButton = [this](const TouchHitGeometry::Rect& r, const char* label, bool selected) {
      if (selected) {
        renderer.fillRect(r.x, r.y, r.width, r.height, true);
      } else {
        renderer.drawRect(r.x, r.y, r.width, r.height, true);
      }
      M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, r.x, r.y, r.width, r.height,
                                  label, !selected,
                                  selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR, 8);
    };
    drawDialogButton(dialog.yes, "删除", deleteConfirmSelected);
    drawDialogButton(dialog.no, "取消", !deleteConfirmSelected);
  }

  const auto labels = mappedInput.mapLabels("« 返回", "选择/长按删除", "向上", "向下");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (firstPaint_) {
    firstPaint_ = false;
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}
