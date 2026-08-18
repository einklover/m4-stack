#pragma once
#include <Epub.h>
#include <Epub/Section.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <string>

#include "EpubReaderMenuActivity.h"
#include "activities/ActivityWithSubactivity.h"

#include "../lib/Epub/Epub/converters/PngToFramebufferConverter.h"

class EpubReaderActivity final : public ActivityWithSubactivity {
  enum class EPUBState {
      READING,
      SETTING,
      LEFT_MARGIN_SETTING,
      RIGHT_MARGIN_SETTING,
      TOP_MARGIN_SETTING, 
      BOTTOM_MARGIN_SETTING,
      END_OF_BOOK_CONFIRM
  };


  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  int pagesUntilFullRefresh = 0;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  bool updateRequired = false;
  bool pendingSubactivityExit = false;  // Defer subactivity exit to avoid use-after-free
  bool pendingGoBack = false;           // Defer go back to avoid use-after-free
  bool pendingGoHome = false;           // Defer go home to avoid race condition with display task
  bool pendingBluetoothSettings = false;  // Defer BT settings open from long-press confirm
  bool skipNextButtonCheck = false;     // Skip button processing for one frame after subactivity exit
  bool globalNextPageMode = false;      // Global next page mode: all buttons trigger page turn
  bool globalNextPageModeToggled = false;  // Flag to prevent repeated triggering of global next page mode
  bool longPressPageBackToggled = false;   // Flag to prevent repeated triggering of long press PageBack
  bool deleteConfirmSelected = false;      // For end-of-book delete confirmation: false=Cancel, true=Delete
  bool automaticPageTurnActive = false;    // Whether automatic page turn is currently active
  bool rollingMode = false;                  // Whether rolling (half-page) auto-turn is active
  bool rollingHalfTurned = false;            // Whether the first half-turn has been rendered
  unsigned long lastPageTurnTime = 0UL;    // Timestamp of the last automatic page turn
  unsigned long pageTurnDuration = 0UL;    // Milliseconds between automatic page turns
  const std::function<void()> onGoBack;
  const std::function<void()> onGoHome;
  const std::string originalSourcePath;      // Original TXT file path if converted from TXT, empty otherwise

  static constexpr int DUAL_PAGE_GUTTER = 8;
  bool isLandscapeDualPage() const;
  // 横屏双页状态
  int dualRightPage = -1;   // 右侧当前显示的页码（与左侧 currentPage 独立）
  bool dualNextLeft = true; // 自动翻页下次更新左侧（true）还是右侧（false）

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();
  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void renderDualContents(std::unique_ptr<Page> leftPage, std::unique_ptr<Page> rightPage,
                          int mt, int mr, int mb, int ml);
  void renderStatusBar(int orientedMarginRight, int orientedMarginBottom,int orientedMarginTop, int orientedMarginLeft) const;
  // Rolling half-page render: top half = next page start, bottom half = current page end.
  // Returns true if render succeeded; false at chapter boundaries (caller falls through to normal render).
  bool renderRollingHalfTurn(int orientedMarginTop, int orientedMarginRight, int orientedMarginBottom,
                             int orientedMarginLeft);
  void saveProgress(int spineIndex, int currentPage, int pageCount);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  // Jump to a normalized percentage of the book (0.0-1.0), preserving full float precision.
  void jumpToPercent(float normalizedPercent);
  void onReaderMenuBack(uint8_t orientation);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void enterReaderMenu(EpubReaderMenuActivity::MenuLayer layer);
  void enterPercentSheet();
  void enterChapterSelector();
  void applyOrientation(uint8_t orientation);
  void applyAutoPageTurnSettings();
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);  // Legacy, calls applyAutoPageTurnSettings

  void renderPngSleepScreen(GfxRenderer& renderer) const;

  static EPUBState state;

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub,
                              const std::function<void()>& onGoBack, const std::function<void()>& onGoHome,
                              const std::string& originalSourcePath = "")
      : ActivityWithSubactivity("EpubReader", renderer, mappedInput),
        epub(std::move(epub)),
        onGoBack(onGoBack),
        onGoHome(onGoHome),
        originalSourcePath(originalSourcePath) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool preventAutoSleep() override { return automaticPageTurnActive; }
  bool isReaderActivity() const override { return true; }
  void onReaderMenuStyleChanged() override {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    section.reset();
    updateRequired = true;
    xSemaphoreGive(renderingMutex);
  }
};