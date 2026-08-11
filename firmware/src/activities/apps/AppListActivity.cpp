#include "AppListActivity.h"

#include <GfxRenderer.h>

#include "AppInstallActivity.h"
#include "AppRuntimeActivity.h"
#include "NativeAppActivity.h"
#include "MappedInputManager.h"
#include "apps/M4xInstaller.h"
#include "apps/M4xPaths.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"
#include "util/M4ListTouchPolicy.h"

#include <algorithm>

namespace {
constexpr unsigned long kAppLongPressMs = 700;

M4ListTouchPolicy::DialogTwoButtonLayout uninstallDialogLayout(const GfxRenderer& renderer) {
  return M4ListTouchPolicy::makeCenteredTwoButtons(renderer.getScreenWidth(), renderer.getScreenHeight() - 190,
                                                   144, 64, 24, 2);
}

TouchHitGeometry::Rect uninstallDataToggleRect(const GfxRenderer& renderer) {
  constexpr int width = 260;
  constexpr int height = 52;
  return {std::max(0, (renderer.getScreenWidth() - width) / 2), renderer.getScreenHeight() - 290, width, height};
}
}  // namespace

AppListActivity::AppListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 const std::function<void()>& onGoBack)
    : ActivityWithSubactivity("AppList", renderer, mappedInput), onGoBack(onGoBack) {}

void AppListActivity::taskTrampoline(void* param) {
  static_cast<AppListActivity*>(param)->displayTaskLoop();
}

void AppListActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired_ && !subActivity) {
      updateRequired_ = false;
      xSemaphoreTake(renderingMutex_, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex_);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void AppListActivity::reload() {
  M4xInstaller::ensureLayout();
  apps_ = M4xRegistry::load();
  if (selectedIndex_ >= static_cast<int>(apps_.size())) {
    selectedIndex_ = std::max(0, static_cast<int>(apps_.size()) - 1);
  }
  mode_ = 0;
}

void AppListActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  renderingMutex_ = xSemaphoreCreateMutex();
  reload();
  updateRequired_ = true;
  xTaskCreate(&AppListActivity::taskTrampoline, "AppList", 4096, this, 1, &displayTaskHandle_);
}

void AppListActivity::onExit() {
  ActivityWithSubactivity::onExit();
  if (renderingMutex_) xSemaphoreTake(renderingMutex_, portMAX_DELAY);
  if (displayTaskHandle_) {
    vTaskDelete(displayTaskHandle_);
    displayTaskHandle_ = nullptr;
  }
  if (renderingMutex_) {
    vSemaphoreDelete(renderingMutex_);
    renderingMutex_ = nullptr;
  }
}

void AppListActivity::openSelected() {
  if (apps_.empty() || selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(apps_.size())) return;
  const auto app = apps_[static_cast<size_t>(selectedIndex_)];
  xSemaphoreTake(renderingMutex_, portMAX_DELAY);
  exitActivity();
  auto onClosed = [this]() {
    exitActivity();
    reload();
    updateRequired_ = true;
  };
  if (app.runtime == M4xRuntimeKind::Native) {
    enterNewActivity(new NativeAppActivity(renderer, mappedInput, app, onClosed));
  } else {
    enterNewActivity(new AppRuntimeActivity(renderer, mappedInput, app, onClosed));
  }
  xSemaphoreGive(renderingMutex_);
}

void AppListActivity::uninstallSelected() {
  if (apps_.empty() || selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(apps_.size())) return;
  std::string err;
  if (!M4xInstaller::uninstall(apps_[static_cast<size_t>(selectedIndex_)].id, uninstallClearData_, err)) {
    Serial.printf("[M4x] uninstall failed: %s\n", err.c_str());
  }
  reload();
  updateRequired_ = true;
}

void AppListActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) {
    if (mode_ == 1) {
      mode_ = 0;
      updateRequired_ = true;
    } else {
      onGoBack();
    }
    return;
  }

  const int count = static_cast<int>(apps_.size());
  if (mode_ == 1) {
    int tx = 0, ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty)) {
      const auto dialog = uninstallDialogLayout(renderer);
      int hit = -1;
      if (M4ListTouchPolicy::dialogButtonFromPoint(dialog, tx, ty, hit)) {
        if (hit == 0) {
          mode_ = 0;
          updateRequired_ = true;
        } else {
          uninstallSelected();
        }
      } else if (uninstallDataToggleRect(renderer).contains(tx, ty)) {
        uninstallClearData_ = !uninstallClearData_;
        updateRequired_ = true;
      }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      uninstallSelected();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      uninstallClearData_ = !uninstallClearData_;
      updateRequired_ = true;
    }
    return;
  }

  if (mappedInput.hasTouch()) {
    int tx = 0, ty = 0;
    const bool tapped = mappedInput.wasScreenTapped(tx, ty);
    if (tapped) {
      const auto metrics = UITheme::getInstance().getMetrics();
      const int pageHeight = renderer.getScreenHeight();
      if (ty >= pageHeight - metrics.buttonHintsHeight) {
        const int quarter = std::max(1, renderer.getScreenWidth() / 4);
        if (tx < quarter) onGoBack();
        else if (tx < quarter * 2) {
          if (count > 0) openSelected();
        } else if (tx < quarter * 3) {
          if (count > 0) {
            mode_ = 1;
            updateRequired_ = true;
          }
        } else {
          xSemaphoreTake(renderingMutex_, portMAX_DELAY);
          exitActivity();
          enterNewActivity(new AppInstallActivity(renderer, mappedInput, "", [this]() {
            exitActivity();
            reload();
            updateRequired_ = true;
          }));
          xSemaphoreGive(renderingMutex_);
        }
        return;
      }

      if (count == 0) {
        xSemaphoreTake(renderingMutex_, portMAX_DELAY);
        exitActivity();
        enterNewActivity(new AppInstallActivity(renderer, mappedInput, "", [this]() {
          exitActivity();
          reload();
          updateRequired_ = true;
        }));
        xSemaphoreGive(renderingMutex_);
        return;
      }

      const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
      const int listHeight = pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
      int hit = -1;
      if (TouchHitGeometry::listIndexFromPoint(ty, listTop, listHeight, metrics.listRowHeight, count,
                                               selectedIndex_, hit)) {
        selectedIndex_ = hit;
        if (mappedInput.lastScreenTouchHeldMs() >= kAppLongPressMs) {
          mode_ = 1;
          updateRequired_ = true;
        } else {
          openSelected();
        }
        return;
      }
    }

    M4ListTouchPolicy::Event te{};
    const auto sw = mappedInput.wasSwipe();
    if (sw == MappedInputManager::SwipeDir::Up) te.swipe = M4ListTouchPolicy::Swipe::Up;
    else if (sw == MappedInputManager::SwipeDir::Down) te.swipe = M4ListTouchPolicy::Swipe::Down;
    if (te.swipe != M4ListTouchPolicy::Swipe::None && count > 0) {
      const auto metrics = UITheme::getInstance().getMetrics();
      const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
      const int listHeight = renderer.getScreenHeight() - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
      const int pageItems = std::max(1, listHeight / metrics.listRowHeight);
      selectedIndex_ = M4ListTouchPolicy::applyPage(selectedIndex_, count, pageItems,
                                                    te.swipe == M4ListTouchPolicy::Swipe::Up);
      updateRequired_ = true;
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (count > 0) openSelected();
    else {
      xSemaphoreTake(renderingMutex_, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new AppInstallActivity(renderer, mappedInput, "", [this]() {
        exitActivity();
        reload();
        updateRequired_ = true;
      }));
      xSemaphoreGive(renderingMutex_);
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left) && count > 0) {
    mode_ = 1;
    updateRequired_ = true;
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    xSemaphoreTake(renderingMutex_, portMAX_DELAY);
    exitActivity();
    enterNewActivity(new AppInstallActivity(renderer, mappedInput, "", [this]() {
      exitActivity();
      reload();
      updateRequired_ = true;
    }));
    xSemaphoreGive(renderingMutex_);
    return;
  }

  if (count > 0) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      selectedIndex_ = (selectedIndex_ + count - 1) % count;
      updateRequired_ = true;
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      selectedIndex_ = (selectedIndex_ + 1) % count;
      updateRequired_ = true;
    }
  }
}

void AppListActivity::render() const {
  renderer.clearScreen();
  const auto metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "应用");

  if (mode_ == 1 && !apps_.empty()) {
    const auto& a = apps_[static_cast<size_t>(selectedIndex_)];
    const auto dialog = uninstallDialogLayout(renderer);
    const auto toggle = uninstallDataToggleRect(renderer);
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, 100, "卸载应用", true, EpdFontFamily::BOLD);
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 155, a.name.c_str());
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 195, "确认移除这个扩展应用？");
    renderer.fillRoundedRect(toggle.x, toggle.y, toggle.width, toggle.height, 10, Color::LightGray);
    M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, toggle.x, toggle.y, toggle.width, toggle.height,
                                uninstallClearData_ ? "同时清除数据：是" : "同时清除数据：否", true,
                                EpdFontFamily::REGULAR, 8);
    const auto drawDialogButton = [&](int index, const char* label) {
      const auto r = dialog.buttonRect(index);
      renderer.fillRoundedRect(r.x, r.y, r.width, r.height, 12, index == 1 ? Color::Black : Color::LightGray);
      M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, r.x, r.y, r.width, r.height, label, index == 0,
                                  EpdFontFamily::BOLD, 8);
    };
    drawDialogButton(0, "取消");
    drawDialogButton(1, "确认卸载");
  } else if (apps_.empty()) {
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, 200, "尚未安装扩展应用", true);
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 260, "将 .m4x 拷到 /apps_inbox/");
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 300, "点按屏幕或确认 安装");
  } else {
    const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int listHeight = pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
    GUI.drawList(renderer, Rect{0, listTop, pageWidth, listHeight}, static_cast<int>(apps_.size()), selectedIndex_,
                 [this](int i) { return apps_[static_cast<size_t>(i)].name; }, nullptr, nullptr,
                 [this](int i) {
                   const auto& a = apps_[static_cast<size_t>(i)];
                   return a.runtime == M4xRuntimeKind::Native ? (std::string("Native · ") + a.version) : a.version;
                 });
  }

  const auto labels = mode_ == 1 ? mappedInput.mapLabels("« 返回", "确认", "", "")
                                 : mappedInput.mapLabels("« 返回", "打开", "卸载", "安装");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
