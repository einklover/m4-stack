#include "RecentBooksActivity.h"

#include <GfxRenderer.h>
#include <SDCardManager.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "apps/M4xRegistry.h"
#include "util/M4ContentProviderContract.h"
#include "util/M4HistoryReopen.h"
#include <SDCardManager.h>
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"
#include "util/M4ListTouchPolicy.h"
#include "util/StringUtils.h"
#include "util/TouchHitGeometry.h"

namespace {
constexpr int SKIP_PAGE_MS = 700;
constexpr unsigned long GO_HOME_MS = 1000;
}  // namespace

void RecentBooksActivity::taskTrampoline(void* param) {
  auto* self = static_cast<RecentBooksActivity*>(param);
  self->displayTaskLoop();
}

void RecentBooksActivity::loadRecentBooks() {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(books.size());

  for (const auto& book : books) {
    // Provider history URIs are not filesystem paths — keep if cache or app marker exists.
    if (M4ContentProvider::isHistoryUri(book.path.c_str())) {
      if (!book.originalSourcePath.empty() && book.originalSourcePath.compare(0, 4, "app:") != 0 &&
          !SdMan.exists(book.originalSourcePath.c_str())) {
        // Drop stale URI only when a former cache path was set and is gone *and*
        // we have no appId hint for reopen.
        if (book.author.find('.') == std::string::npos) continue;
      }
      recentBooks.push_back(book);
      continue;
    }
    if (!SdMan.exists(book.path.c_str())) {
      continue;
    }
    recentBooks.push_back(book);
  }
}

void RecentBooksActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  // Load data
  loadRecentBooks();

  selectorIndex = 0;
  updateRequired = true;

  xTaskCreate(&RecentBooksActivity::taskTrampoline, "RecentBooksActivityTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void RecentBooksActivity::onExit() {
  Activity::onExit();

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;

  recentBooks.clear();
}

void RecentBooksActivity::loop() {
  const bool upReleased = mappedInput.wasReleased(MappedInputManager::Button::Left) ||
                          mappedInput.wasReleased(MappedInputManager::Button::Up);
  const bool downReleased = mappedInput.wasReleased(MappedInputManager::Button::Right) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Down);

  const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;
  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, true);
  const int listSize = static_cast<int>(recentBooks.size());

  // Touch: same list geometry as render() drawList
  if (mappedInput.hasTouch()) {
    auto metrics = UITheme::getInstance().getMetrics();
    const int pageHeight = renderer.getScreenHeight();
    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

    M4ListTouchPolicy::Event te{};
    te.backGesture = mappedInput.wasBackGesture();
    const auto sw = mappedInput.wasSwipe();
    if (sw == MappedInputManager::SwipeDir::Up) te.swipe = M4ListTouchPolicy::Swipe::Up;
    else if (sw == MappedInputManager::SwipeDir::Down) te.swipe = M4ListTouchPolicy::Swipe::Down;
    else if (sw == MappedInputManager::SwipeDir::Left) te.swipe = M4ListTouchPolicy::Swipe::Left;
    else if (sw == MappedInputManager::SwipeDir::Right) te.swipe = M4ListTouchPolicy::Swipe::Right;
    int downX = 0, downY = 0, tapX = 0, tapY = 0;
    te = M4ListTouchPolicy::mergeFrame(te.backGesture, te.swipe,
                                       mappedInput.wasScreenTouchDown(downX, downY), downX, downY,
                                       mappedInput.wasScreenTapped(tapX, tapY), tapX, tapY);

    M4ListTouchPolicy::ListLayout layout;
    layout.listTop = contentTop;
    layout.listHeight = contentHeight;
    layout.rowStep = metrics.listRowHeight;
    layout.itemCount = listSize;
    layout.selectedIndex = selectorIndex;
    layout.maxVisible = 0;

    int hit = -1;
    const auto act = M4ListTouchPolicy::resolveList(te, layout, hit);
    if (act == M4ListTouchPolicy::Action::Back) {
      onGoHome();
      return;
    }
    if (act == M4ListTouchPolicy::Action::PageDown || act == M4ListTouchPolicy::Action::PageUp) {
      if (listSize > 0) {
        selectorIndex = M4ListTouchPolicy::applyPage(selectorIndex, listSize, pageItems,
                                                     act == M4ListTouchPolicy::Action::PageDown);
        updateRequired = true;
      }
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
      {
        const auto& b = recentBooks[selectorIndex];
        const auto apps = M4xRegistry::load();
        const M4HistoryReopen::ProviderAppIdResolver appIdForProvider = [apps](const std::string& providerId) {
          std::string found;
          for (const auto& app : apps) {
            if (app.provider != providerId) continue;
            if (!found.empty() && found != app.id) return std::string();
            found = app.id;
          }
          return found;
        };
        const std::string src = M4HistoryReopen::appHintForRecentBook(
            b.path, b.originalSourcePath, b.author, appIdForProvider);
        onSelectBook(b.path, src);
      }
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!recentBooks.empty() && selectorIndex < static_cast<int>(recentBooks.size())) {
      Serial.printf("Selected recent book: %s\n", recentBooks[selectorIndex].path.c_str());
      {
        const auto& b = recentBooks[selectorIndex];
        const auto apps = M4xRegistry::load();
        const M4HistoryReopen::ProviderAppIdResolver appIdForProvider = [apps](const std::string& providerId) {
          std::string found;
          for (const auto& app : apps) {
            if (app.provider != providerId) continue;
            if (!found.empty() && found != app.id) return std::string();
            found = app.id;
          }
          return found;
        };
        const std::string src = M4HistoryReopen::appHintForRecentBook(
            b.path, b.originalSourcePath, b.author, appIdForProvider);
        onSelectBook(b.path, src);
      }
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
  }

  if (listSize <= 0) return;
  if (upReleased) {
    if (skipPage) {
      selectorIndex = std::max(static_cast<int>((selectorIndex / pageItems - 1) * pageItems), 0);
    } else {
      selectorIndex = (selectorIndex + listSize - 1) % listSize;
    }
    updateRequired = true;
  } else if (downReleased) {
    if (skipPage) {
      selectorIndex = std::min(static_cast<int>((selectorIndex / pageItems + 1) * pageItems), listSize - 1);
    } else {
      selectorIndex = (selectorIndex + 1) % listSize;
    }
    updateRequired = true;
  }
}

void RecentBooksActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void RecentBooksActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "阅读历史");

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  // Recent tab
  if (recentBooks.empty()) {
    M4UiText::draw(renderer, UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, "没有阅读历史");
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, recentBooks.size(), selectorIndex,
        [this](int index) { return recentBooks[index].title; },
        nullptr,
        [](int index) -> UIIcon { return UIIcon::Book; },  // All items are books
        nullptr);
  }

  // Help text
  const auto labels = mappedInput.mapLabels("« 主页", "打开", "向上", "向下");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
