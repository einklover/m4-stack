#include "XtcReaderChapterSelectionActivity.h"

#include <algorithm>
#include <EpdFontLoader.h>
#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "Xtc.h"
#include "util/M4ListTouchPolicy.h"
#include "util/M4TouchListMetrics.h"
#include "util/M4UiText.h"

//目录跟随旋转
#include "CrossPointSettings.h"

namespace {
constexpr int SKIP_PAGE_MS = 700;
int page = 1;

M4TouchListMetrics::ChapterListLayout chapterLayout(const GfxRenderer& renderer, bool touch) {
  const int layoutFont = touch ? UI_12_FONT_ID : UI_10_FONT_ID;
  return M4TouchListMetrics::makeChapterListLayout(
      renderer.getScreenWidth(), renderer.getScreenHeight(), touch,
      static_cast<TouchHitGeometry::Orientation>(renderer.getOrientation()),
      M4UiText::systemListLineHeight(renderer, layoutFont));
}
}  // namespace

int XtcReaderChapterSelectionActivity::getPageItems() const {
  const bool touch = mappedInput.hasTouch();
  const auto layout = chapterLayout(renderer, touch);
  return std::max(1, layout.list.height / layout.rowHeight);
}

void XtcReaderChapterSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<XtcReaderChapterSelectionActivity*>(param);
  self->displayTaskLoop();
}

void XtcReaderChapterSelectionActivity::onEnter() {
  renderer.clearScreen();
  Activity::onEnter();
  M4TouchNavigation::activateForChapterSelection();

  // Full-CJK titles; skip rescan if already loaded this session.
  EpdFontLoader::ensureFontsFromSd(renderer);

  // 屏幕方向配置
  switch (SETTINGS.orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  };


  updateRequired = true;
  //循环找所在章节

 selectorIndex = xtc->getchapter(currentPage); 
 page = selectorIndex/getPageItems()+1;

  xTaskCreate(&XtcReaderChapterSelectionActivity::taskTrampoline, "XtcReaderChapterSelectionTask",
              4096,        
              this,        
              1,           
              &displayTaskHandle
  );
}

void XtcReaderChapterSelectionActivity::onExit() {
  Activity::onExit();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
}

void XtcReaderChapterSelectionActivity::loop() {
  const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);

  const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;
  const int pageItems = getPageItems();
  const int totalChapters = xtc ? static_cast<int>(xtc->getmaxchapter()) : 0;

  // Touch: chapterListTop + chapterLineHeight (matches renderScreen).
  if (mappedInput.hasTouch() && totalChapters > 0) {
    const auto chapterFrame = chapterLayout(renderer, true);
    const int BASE_Y = chapterFrame.list.y;
    const int FIX_LINE_HEIGHT = chapterFrame.rowHeight;
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
    layout.listTop = BASE_Y;
    layout.listHeight = pageItems * FIX_LINE_HEIGHT;
    layout.rowStep = FIX_LINE_HEIGHT;
    layout.itemCount = totalChapters;
    layout.selectedIndex = selectorIndex;
    layout.maxVisible = 0;
    int hit = -1;
    const auto act = M4ListTouchPolicy::resolveList(te, layout, hit);
    if (act == M4ListTouchPolicy::Action::Back) {
      onGoBack();
      return;
    }
    if (act == M4ListTouchPolicy::Action::PageDown || act == M4ListTouchPolicy::Action::PageUp) {
      selectorIndex = M4ListTouchPolicy::applyPage(selectorIndex, totalChapters, pageItems,
                                                   act == M4ListTouchPolicy::Action::PageDown);
      page = selectorIndex / pageItems + 1;
      updateRequired = true;
      return;
    }
    if (act == M4ListTouchPolicy::Action::Select && hit >= 0) {
      if (selectorIndex != hit) {
        selectorIndex = hit;
        page = selectorIndex / pageItems + 1;
        updateRequired = true;
      }
      return;
    }
    if (act == M4ListTouchPolicy::Action::Activate && hit >= 0) {
      selectorIndex = hit;
      page = selectorIndex / pageItems + 1;
      const int pagebegin = (page - 1) * pageItems;
      xtc->readChapters_gd(pagebegin);
      uint32_t chapterpage = this->xtc->getChapterstartpage(selectorIndex);
      Serial.printf("[%lu] [XTC] 触摸跳转章节：%d,跳转页数：%d\n", millis(), selectorIndex, chapterpage);
      onSelectPage(chapterpage);
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const int pagebegin=(page-1)*getPageItems();
    xtc->readChapters_gd(pagebegin);
    uint32_t chapterpage = this->xtc->getChapterstartpage(selectorIndex);
    Serial.printf("[%lu] [XTC] 跳转章节：%d,跳转页数：%d\n", millis(), selectorIndex, chapterpage);
    
    onSelectPage(chapterpage);
    // 确认按键逻辑，按需补充
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoBack();
  } else if (prevReleased) {
    bool isUpKey = mappedInput.wasReleased(MappedInputManager::Button::Up);
    if (skipPage || isUpKey) {
      page -= 1;
      if(page < 1) page = 1; 
      selectorIndex = (page-1)*getPageItems(); 
    } else {
      selectorIndex--; 
      if(selectorIndex < 0) selectorIndex = 0; 
    }
    updateRequired = true;
  } else if (nextReleased) {
    bool isDownKey = mappedInput.wasReleased(MappedInputManager::Button::Down);
    const int totalChapters = (int)xtc->getmaxchapter();
    if (skipPage || isDownKey) {
      page += 1;
      selectorIndex = (page-1)*getPageItems();
    } else {
      selectorIndex++;
    }
    // 限制 selectorIndex 不超过实际章节数
    if (selectorIndex >= totalChapters) selectorIndex = totalChapters - 1;
    updateRequired = true;
  }
}

void XtcReaderChapterSelectionActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      renderScreen();
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void XtcReaderChapterSelectionActivity::renderScreen() {
  // Keep standalone XTC chapter-list entry consistent with EPUB/TXT: clear
  // and redraw the whole logical frame before the full-frame FAST submit.
  renderer.clearScreen();
  const int pagebegin=(page-1)*getPageItems();
  int page_chapter=getPageItems();
  static int parsedPage = -1; // ✅ 保留页码缓存，只解析1次

  if (parsedPage != page) {
    xtc->readChapters_gd(pagebegin);
    parsedPage = page;
  }

  const int totalChapters = (int)xtc->getmaxchapter(); // 实际总章节数

  const bool touch = mappedInput.hasTouch();
  const int layoutFont = touch ? UI_12_FONT_ID : UI_10_FONT_ID;
  const auto chapterFrame = chapterLayout(renderer, touch);
  const int FIX_LINE_HEIGHT = chapterFrame.rowHeight;
  const int BASE_Y = chapterFrame.list.y;
  GUI.drawHeader(renderer, Rect{chapterFrame.header.x, chapterFrame.header.y,
                                chapterFrame.header.width, chapterFrame.header.height}, "目录");

  for (int i = pagebegin; i <= pagebegin + page_chapter - 1 && i < totalChapters; i++) {
      int localIdx = i - pagebegin; 
      
      uint32_t currOffset = this->xtc->getChapterstartpage(i); 
      std::string dirTitle = this->xtc->getChapterTitleByIndex(i); 
      
      Serial.printf("[%lu] [XTC_CHAPTER] 第%d章，名字为:%s,页码为%d\n", millis(), i, dirTitle.c_str(),currOffset);
      static char title[64];
      strncpy(title, dirTitle.c_str(), sizeof(title)-1);
      title[sizeof(title)-1] = '\0';
      
      int drawY = BASE_Y + localIdx * FIX_LINE_HEIGHT;
      if (i == selectorIndex) {
        renderer.fillRect(0, drawY, renderer.getScreenWidth(), FIX_LINE_HEIGHT);
        M4UiText::drawSystem(renderer, layoutFont, 20,
                             drawY + (FIX_LINE_HEIGHT - chapterFrame.systemLineHeight) / 2,
                             title, false, EpdFontFamily::REGULAR);
      } else {
        if (touch) renderer.drawRect(4, drawY + 2, renderer.getScreenWidth() - 8, FIX_LINE_HEIGHT - 4);
        M4UiText::drawSystem(renderer, layoutFont, 20,
                             drawY + (FIX_LINE_HEIGHT - chapterFrame.systemLineHeight) / 2,
                             title, true, EpdFontFamily::REGULAR);
      }
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
