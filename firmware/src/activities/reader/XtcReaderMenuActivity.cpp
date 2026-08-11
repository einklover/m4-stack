#include "XtcReaderMenuActivity.h"

#include <algorithm>
#include <GfxRenderer.h>

#include "AutoPageTurnIntervalActivity.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"
#include "util/M4ListTouchPolicy.h"
#include "util/M4TouchListMetrics.h"

void XtcReaderMenuActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();
  updateRequired = true;
  xTaskCreate(&XtcReaderMenuActivity::taskTrampoline, "XtcMenuTask", 4096, this, 1, &displayTaskHandle);
}

void XtcReaderMenuActivity::onExit() {
  ActivityWithSubactivity::onExit();
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void XtcReaderMenuActivity::taskTrampoline(void* param) {
  auto* self = static_cast<XtcReaderMenuActivity*>(param);
  self->displayTaskLoop();
}

void XtcReaderMenuActivity::displayTaskLoop() {
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

void XtcReaderMenuActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  auto runSelectedAction = [this]() {
    const auto selectedAction = menuItems[selectedIndex].action;

    // 阅读方向：原地循环，返回时应用
    if (selectedAction == MenuAction::ROTATE_SCREEN) {
      pendingOrientation = (pendingOrientation + 1) % (uint8_t)orientationLabels.size();
      updateRequired = true;
      return;
    }

    // 自动翻页：切换开关并保存
    if (selectedAction == MenuAction::AUTO_PAGE_TURN) {
      SETTINGS.autoPageTurnEnabled = SETTINGS.autoPageTurnEnabled ? 0 : 1;
      SETTINGS.saveToFile();
      updateRequired = true;
      return;
    }

    // 翻页方式：切换并保存
    if (selectedAction == MenuAction::PAGE_TURN_MODE) {
      SETTINGS.autoPageTurnMode = SETTINGS.autoPageTurnMode ? 0 : 1;
      SETTINGS.saveToFile();
      updateRequired = true;
      return;
    }

    // 翻页间隔：打开选择器
    if (selectedAction == MenuAction::PAGE_TURN_INTERVAL) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      enterNewActivity(new AutoPageTurnIntervalActivity(
          renderer, mappedInput, SETTINGS.autoPageTurnInterval,
          [this](const int interval) {
            SETTINGS.autoPageTurnInterval = interval;
            SETTINGS.saveToFile();
            exitActivity();
            updateRequired = true;
          },
          [this]() {
            exitActivity();
            updateRequired = true;
          }));
      xSemaphoreGive(renderingMutex);
      return;
    }

    // 抗锯齿：原地切换，返回时应用
    if (selectedAction == MenuAction::ANTI_ALIAS) {
      pendingAntiAlias = !pendingAntiAlias;
      updateRequired = true;
      return;
    }

    // 其他动作：直接回调（GO_TO_PERCENT / DELETE_CACHE / GO_HOME）
    auto actionCallback = onAction;
    actionCallback(selectedAction);
  };

  // Touch: readerMenuListTop + readerMenuLineHeight (matches renderScreen)
  if (mappedInput.hasTouch()) {
    if (mappedInput.wasBackGesture()) {
      onBack(pendingOrientation, pendingAntiAlias);
      return;
    }
    const auto orientation = renderer.getOrientation();
    const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
    const int contentY = isPortraitInverted ? 50 : 0;
    const int lineHeight = M4TouchListMetrics::readerMenuLineHeight(true);
    const int startY = M4TouchListMetrics::readerMenuListTop(true, contentY);
    const int totalItems = static_cast<int>(menuItems.size());
    M4ListTouchPolicy::Event te{};
    const auto sw = mappedInput.wasSwipe();
    if (sw == MappedInputManager::SwipeDir::Up) te.swipe = M4ListTouchPolicy::Swipe::Up;
    else if (sw == MappedInputManager::SwipeDir::Down) te.swipe = M4ListTouchPolicy::Swipe::Down;
    int dx = 0, dy = 0, tx = 0, ty = 0;
    te = M4ListTouchPolicy::mergeFrame(false, te.swipe, mappedInput.wasScreenTouchDown(dx, dy), dx, dy,
                                       mappedInput.wasScreenTapped(tx, ty), tx, ty);
    M4ListTouchPolicy::ListLayout layout;
    const int screenH = renderer.getScreenHeight();
    const int listHeight = std::max(lineHeight, screenH - startY - 40);
    layout.listTop = startY;
    layout.listHeight = listHeight;
    layout.rowStep = lineHeight;
    layout.itemCount = totalItems;
    layout.selectedIndex = selectedIndex;
    const int pageItems = std::max(1, listHeight / lineHeight);
    int hit = -1;
    const auto act = M4ListTouchPolicy::resolveList(te, layout, hit);
    if (act == M4ListTouchPolicy::Action::PageDown || act == M4ListTouchPolicy::Action::PageUp) {
      selectedIndex = M4ListTouchPolicy::applyPage(selectedIndex, totalItems, pageItems,
                                                   act == M4ListTouchPolicy::Action::PageDown);
      updateRequired = true;
      return;
    }
    if (act == M4ListTouchPolicy::Action::Select && hit >= 0) {
      if (selectedIndex != hit) {
        selectedIndex = hit;
        updateRequired = true;
      }
      return;
    }
    if (act == M4ListTouchPolicy::Action::Activate && hit >= 0) {
      selectedIndex = hit;
      runSelectedAction();
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
      mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    selectedIndex = (selectedIndex + (int)menuItems.size() - 1) % (int)menuItems.size();
    updateRequired = true;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
             mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    selectedIndex = (selectedIndex + 1) % (int)menuItems.size();
    updateRequired = true;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    runSelectedAction();
    return;

  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // 退出菜单，把待应用的值回传给 XtcReaderActivity
    onBack(pendingOrientation, pendingAntiAlias);
    return;
  }
}

void XtcReaderMenuActivity::renderScreen() {
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

  // 书名标题
  const std::string truncTitle =
      M4UiText::truncated(renderer, UI_12_FONT_ID, title.c_str(), contentWidth - 40, EpdFontFamily::BOLD);
  const int titleX =
      contentX + (contentWidth - M4UiText::textWidth(renderer, UI_12_FONT_ID, truncTitle.c_str(), EpdFontFamily::BOLD)) / 2;
  M4UiText::draw(renderer, UI_12_FONT_ID, titleX, 15 + contentY, truncTitle.c_str(), true, EpdFontFamily::BOLD);

  // 进度摘要
  std::string progressLine;
  if (totalPages > 0) {
    progressLine = "第 " + std::to_string(currentPage + 1) + "/" + std::to_string(totalPages) + " 页  |  ";
  }
  progressLine += "进度: " + std::to_string(bookProgressPercent) + "%";
  M4UiText::drawCentered(renderer, UI_10_FONT_ID, 45 + contentY, progressLine.c_str());

  // 菜单列表（触屏加大行高；超出一屏时按 selectedIndex 分页绘制）
  const bool touch = mappedInput.hasTouch();
  const int startY = M4TouchListMetrics::readerMenuListTop(touch, contentY);
  const int lineHeight = M4TouchListMetrics::readerMenuLineHeight(touch);
  const int rowFont = touch ? UI_12_FONT_ID : UI_10_FONT_ID;
  const int pageHeight = renderer.getScreenHeight();
  const int listHeight = std::max(lineHeight, pageHeight - startY - 40);
  const int pageItems = std::max(1, listHeight / lineHeight);
  const int totalItems = static_cast<int>(menuItems.size());
  const int pageStart = (selectedIndex / pageItems) * pageItems;

  for (int i = pageStart; i < totalItems && i < pageStart + pageItems; ++i) {
    const int displayY = startY + (i - pageStart) * lineHeight;
    const bool isSelected = (i == selectedIndex);

    if (isSelected) {
      renderer.fillRect(contentX, displayY, contentWidth - 1, lineHeight, true);
    } else if (touch) {
      renderer.drawRect(contentX + 2, displayY + 2, contentWidth - 5, lineHeight - 4);
    }

    renderer.drawText(rowFont, contentX + 20, displayY + (touch ? 10 : 0), menuItems[i].label.c_str(), !isSelected);

    // 阅读方向：右侧显示当前值
    if (menuItems[i].action == MenuAction::ROTATE_SCREEN) {
      const auto value = orientationLabels[pendingOrientation];
      const auto width = M4UiText::textWidth(renderer, UI_10_FONT_ID, value);
      M4UiText::draw(renderer, UI_10_FONT_ID, contentX + contentWidth - 20 - width, displayY + (touch ? 10 : 0), value,
                        !isSelected);
    }

    // 自动翻页：右侧显示开/关
    if (menuItems[i].action == MenuAction::AUTO_PAGE_TURN) {
      const auto value = autoPageTurnLabels[SETTINGS.autoPageTurnEnabled ? 1 : 0];
      const auto width = M4UiText::textWidth(renderer, UI_10_FONT_ID, value);
      M4UiText::draw(renderer, UI_10_FONT_ID, contentX + contentWidth - 20 - width, displayY + (touch ? 10 : 0), value,
                        !isSelected);
    }

    // 翻页方式：右侧显示当前模式
    if (menuItems[i].action == MenuAction::PAGE_TURN_MODE) {
      const auto value = pageTurnModeLabels[SETTINGS.autoPageTurnMode ? 1 : 0];
      const auto width = M4UiText::textWidth(renderer, UI_10_FONT_ID, value);
      M4UiText::draw(renderer, UI_10_FONT_ID, contentX + contentWidth - 20 - width, displayY + (touch ? 10 : 0), value,
                        !isSelected);
    }

    // 翻页间隔：右侧显示秒数
    if (menuItems[i].action == MenuAction::PAGE_TURN_INTERVAL) {
      const std::string value = std::to_string(SETTINGS.autoPageTurnInterval) + "秒";
      const auto width = M4UiText::textWidth(renderer, UI_10_FONT_ID, value.c_str());
      M4UiText::draw(renderer, UI_10_FONT_ID, contentX + contentWidth - 20 - width, displayY + (touch ? 10 : 0),
                        value.c_str(), !isSelected);
    }

    // 抗锯齿：右侧显示开/关
    if (menuItems[i].action == MenuAction::ANTI_ALIAS) {
      const char* value = pendingAntiAlias ? "开" : "关";
      const auto width = M4UiText::textWidth(renderer, UI_10_FONT_ID, value);
      M4UiText::draw(renderer, UI_10_FONT_ID, contentX + contentWidth - 20 - width, displayY + (touch ? 10 : 0), value,
                        !isSelected);
    }
  }

  // 底部按键提示
  const auto labels = mappedInput.mapLabels("« 返回", "选择", "向上", "向下");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
