/**
 * XtcReaderActivity.h
 *
 * XTC ebook reader activity for CrossPoint Reader
 * Displays pre-rendered XTC pages on e-ink display
 */

#pragma once

#include <Xtc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "activities/ActivityWithSubactivity.h"
#include "XtcReaderMenuActivity.h"
#include "EpubReaderPercentSelectionActivity.h"

class XtcReaderActivity final : public ActivityWithSubactivity {
  std::shared_ptr<Xtc> xtc;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  uint32_t currentPage = 0;
  int pagesUntilFullRefresh = 0;
  bool updateRequired = false;
  const std::function<void()> onGoBack;
  const std::function<void()> onGoHome;

  // reusable page buffer to avoid malloc/free on every page
  uint8_t* pageBuffer = nullptr;
  size_t pageBufferCapacity = 0;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();
  bool isRenderUpperHalf;
  void renderPage(bool isRenderUpperHalf);
  void saveProgress() const;
  void loadProgress();
    //分批缓存
  uint32_t m_loadedMax = 499;
  void renderFullPage();
  //防止调用失败
  void drawSliceContent(int sliceN, bool isGrayscale, bool isLSB);

  // 菜单相关
  bool pendingGoBack = false;
  bool pendingGoHome = false;
  bool automaticPageTurnActive = false;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;

  void onReaderMenuBack(uint8_t orientation, bool antiAlias);
  void onReaderMenuConfirm(XtcReaderMenuActivity::MenuAction action);
  void applyOrientation(uint8_t orientation);
  void applyAutoPageTurnSettings();
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);  // Legacy

 public:
  explicit XtcReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Xtc> xtc,
                             const std::function<void()>& onGoBack, const std::function<void()>& onGoHome)
      : ActivityWithSubactivity("XtcReader", renderer, mappedInput),
        xtc(std::move(xtc)),
        onGoBack(onGoBack),
        onGoHome(onGoHome) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool preventAutoSleep() override { return automaticPageTurnActive; }
  bool isReaderActivity() const override { return true; }
  void gotoPage(uint32_t targetPage);
};
