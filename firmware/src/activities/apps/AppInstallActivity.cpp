#include "AppInstallActivity.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "apps/M4xPaths.h"
#include "apps/M4xRegistry.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"
#include "util/M4ListTouchPolicy.h"

#include <SDCardManager.h>
#include <algorithm>
#include <atomic>
#include <cstdio>

namespace {
void primaryButtonRect(int screenW, int screenH, int& x, int& y, int& w, int& h) {
  w = std::min(360, screenW - 40);
  h = 64;
  x = (screenW - w) / 2;
  y = screenH - 140;
}

bool pointInRect(int px, int py, int x, int y, int w, int h) {
  return px >= x && px < x + w && py >= y && py < y + h;
}

// Worker task: install must not run on the main loop stack (miniz/zip + SD can overflow).
struct InstallJob {
  std::string path;
  M4xInstallResult result;
  // Worker publishes done; UI loop polls — needs release/acquire, not volatile.
  std::atomic<bool> done{false};
};

void installTaskTrampoline(void* param) {
  auto* job = static_cast<InstallJob*>(param);
  Serial.printf("[M4x] install task start heap=%u\n", static_cast<unsigned>(ESP.getFreeHeap()));
  job->result = M4xInstaller::install(job->path);
  job->done.store(true, std::memory_order_release);
  Serial.printf("[M4x] install task done ok=%d err=%s\n", job->result.ok ? 1 : 0, job->result.error.c_str());
  vTaskDelete(nullptr);
}
}  // namespace

AppInstallActivity::AppInstallActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       std::string packagePath, const std::function<void()>& onDone)
    : ActivityWithSubactivity("AppInstall", renderer, mappedInput),
      packagePath_(std::move(packagePath)),
      onDone_(onDone) {}

void AppInstallActivity::taskTrampoline(void* param) {
  static_cast<AppInstallActivity*>(param)->displayTaskLoop();
}

void AppInstallActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired_) {
      updateRequired_ = false;
      xSemaphoreTake(renderingMutex_, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex_);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void AppInstallActivity::scanInbox() {
  inboxPackages_.clear();
  M4xInstaller::ensureLayout();
  const auto files = SdMan.listFiles(M4xPaths::kInbox, 64);
  for (const auto& fArduino : files) {
    const std::string f = fArduino.c_str();
    if (f.size() >= 4) {
      const auto ext = f.substr(f.size() - 4);
      if (ext == ".m4x" || ext == ".M4X") {
        std::string full = M4xPaths::kInbox;
        full += "/";
        full += f;
        inboxPackages_.push_back(std::move(full));
      }
    }
  }
}

void AppInstallActivity::probeSelected() {
  std::string path = packagePath_;
  if (path.empty()) {
    if (inboxPackages_.empty() || selectedIndex_ < 0 ||
        selectedIndex_ >= static_cast<int>(inboxPackages_.size())) {
      probe_ = {};
      probe_.message = "收件箱没有 .m4x 安装包";
      stage_ = Stage::Result;
      resultMessage_ = probe_.message;
      return;
    }
    path = inboxPackages_[static_cast<size_t>(selectedIndex_)];
  }
  Serial.printf("[M4x] probeSelected path=%s\n", path.c_str());
  probe_ = M4xInstaller::probe(path);
  packagePath_ = path;
  if (probe_.ok) {
    stage_ = Stage::Confirm;
  } else {
    stage_ = Stage::Result;
    resultMessage_ = probe_.message;
  }
}

void AppInstallActivity::doInstall() {
  if (installRunning_) return;
  installRunning_ = true;
  stage_ = Stage::Result;
  resultMessage_ = "正在安装...";
  updateRequired_ = true;

  // Heap-allocated job outlives this call; task fills result.
  auto* job = new InstallJob();
  job->path = packagePath_;
  job->done = false;

  // Large stack: ZipFile + JSON + SD on worker, not UI loop.
  const BaseType_t ok =
      xTaskCreate(installTaskTrampoline, "M4xInstall", 12288, job, 1, nullptr);
  if (ok != pdPASS) {
    delete job;
    installRunning_ = false;
    resultMessage_ = "无法创建安装任务(内存不足)";
    updateRequired_ = true;
    return;
  }

  // Poll completion without blocking forever (watchdog-safe).
  const unsigned long start = millis();
  while (!job->done.load(std::memory_order_acquire)) {
    vTaskDelay(20 / portTICK_PERIOD_MS);
    if (millis() - start > 60000) {
      resultMessage_ = "安装超时";
      installRunning_ = false;
      // Leak job if task still running — better than use-after-free.
      updateRequired_ = true;
      return;
    }
  }

  probe_ = std::move(job->result);
  delete job;
  installRunning_ = false;
  stage_ = Stage::Result;
  if (probe_.ok) {
    char okLine[128];
    std::snprintf(okLine, sizeof(okLine), "安装成功: %s %s (%d)", probe_.manifest.name.c_str(),
                  probe_.manifest.version.c_str(), probe_.manifest.versionCode);
    resultMessage_ = okLine;
  } else {
    resultMessage_ = probe_.message;
  }
  updateRequired_ = true;
}

void AppInstallActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  renderingMutex_ = xSemaphoreCreateMutex();
  installRunning_ = false;
  M4xInstaller::ensureLayout();
  if (!packagePath_.empty()) {
    probeSelected();
  } else {
    scanInbox();
    stage_ = Stage::Pick;
    if (inboxPackages_.size() == 1) {
      selectedIndex_ = 0;
      probeSelected();
    }
  }
  updateRequired_ = true;
  xTaskCreate(&AppInstallActivity::taskTrampoline, "AppInstallUI", 4096, this, 1, &displayTaskHandle_);
}

void AppInstallActivity::onExit() {
  ActivityWithSubactivity::onExit();
  // Wait briefly if install still running
  unsigned long t0 = millis();
  while (installRunning_ && millis() - t0 < 2000) {
    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
  xSemaphoreTake(renderingMutex_, portMAX_DELAY);
  if (displayTaskHandle_) {
    vTaskDelete(displayTaskHandle_);
    displayTaskHandle_ = nullptr;
  }
  vSemaphoreDelete(renderingMutex_);
  renderingMutex_ = nullptr;
}

void AppInstallActivity::loop() {
  if (installRunning_) return;  // ignore input during install worker

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) {
    onDone_();
    return;
  }

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  int btnX = 0, btnY = 0, btnW = 0, btnH = 0;
  primaryButtonRect(pageWidth, pageHeight, btnX, btnY, btnW, btnH);

  auto confirmPressed = [&]() -> bool {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) return true;
    if (!mappedInput.hasTouch()) return false;
    int tx = 0, ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty)) return true;
    if (mappedInput.wasScreenTouchDown(tx, ty) && pointInRect(tx, ty, btnX, btnY, btnW, btnH)) return true;
    return false;
  };

  if (stage_ == Stage::Result) {
    if (resultMessage_ == "正在安装...") return;
    if (confirmPressed()) onDone_();
    return;
  }

  if (stage_ == Stage::Confirm) {
    if (confirmPressed()) doInstall();
    return;
  }

  const int count = static_cast<int>(inboxPackages_.size());
  if (mappedInput.hasTouch() && count > 0) {
    auto metrics = UITheme::getInstance().getMetrics();
    const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int listHeight = pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
    M4ListTouchPolicy::Event te{};
    int dx = 0, dy = 0, tx = 0, ty = 0;
    te = M4ListTouchPolicy::mergeFrame(false, M4ListTouchPolicy::Swipe::None,
                                       mappedInput.wasScreenTouchDown(dx, dy), dx, dy,
                                       mappedInput.wasScreenTapped(tx, ty), tx, ty);
    M4ListTouchPolicy::ListLayout layout;
    layout.listTop = listTop;
    layout.listHeight = listHeight;
    layout.rowStep = metrics.listRowHeight;
    layout.itemCount = count;
    layout.selectedIndex = selectedIndex_;
    int hit = -1;
    const auto act = M4ListTouchPolicy::resolveList(te, layout, hit);
    if (act == M4ListTouchPolicy::Action::Select && hit >= 0) {
      selectedIndex_ = hit;
      updateRequired_ = true;
      return;
    }
    if (act == M4ListTouchPolicy::Action::Activate && hit >= 0) {
      selectedIndex_ = hit;
      probeSelected();
      updateRequired_ = true;
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && count > 0) {
    probeSelected();
    updateRequired_ = true;
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

void AppInstallActivity::render() const {
  renderer.clearScreen();
  auto metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "安装扩展");

  int btnX = 0, btnY = 0, btnW = 0, btnH = 0;
  primaryButtonRect(pageWidth, pageHeight, btnX, btnY, btnW, btnH);

  if (stage_ == Stage::Result) {
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, 220, resultMessage_.c_str(), true, EpdFontFamily::BOLD);
    if (resultMessage_ != "正在安装...") {
      renderer.fillRect(btnX, btnY, btnW, btnH, true);
      M4UiText::drawCentered(renderer, UI_12_FONT_ID, btnY + 28, "完成", false, EpdFontFamily::BOLD);
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, btnY - 30, "点按按钮或屏幕返回");
    }
  } else if (stage_ == Stage::Confirm && probe_.ok) {
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, 90, probe_.manifest.name.c_str(), true, EpdFontFamily::BOLD);
    // Always show *package* version (incoming), not a stale registry-only label.
    char line[120];
    std::snprintf(line, sizeof(line), "版本 %s (%d)", probe_.manifest.version.c_str(),
                  probe_.manifest.versionCode);
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 130, line);
    // Same-id upgrade: show installed -> package so RC bumps are visible.
    {
      auto apps = M4xRegistry::load();
      if (const auto* existing = M4xRegistry::find(apps, probe_.manifest.id)) {
        char up[140];
        std::snprintf(up, sizeof(up), "升级 %s (%d) -> %s (%d)", existing->version.c_str(),
                      existing->versionCode, probe_.manifest.version.c_str(),
                      probe_.manifest.versionCode);
        M4UiText::drawCentered(renderer, UI_10_FONT_ID, 160, up);
      }
    }
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 190, probe_.manifest.id.c_str());
    if (!probe_.manifest.description.empty()) {
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, 220, probe_.manifest.description.c_str());
    }
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 270, "权限:");
    int y = 300;
    for (const auto& p : probe_.manifest.permissions) {
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, y, p.c_str());
      y += 28;
      if (y > btnY - 40) break;
    }
    renderer.fillRect(btnX, btnY, btnW, btnH, true);
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, btnY + 28, "确认安装", false, EpdFontFamily::BOLD);
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, btnY - 28, "点按按钮或屏幕任意处安装");
  } else {
    if (inboxPackages_.empty()) {
      M4UiText::drawCentered(renderer, UI_12_FONT_ID, 220, "收件箱为空", true);
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, 280, "请将 .m4x 复制到");
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, 320, "/apps_inbox/");
    } else {
      const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
      const int listHeight = pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing - 80;
      const int count = static_cast<int>(inboxPackages_.size());
      GUI.drawList(
          renderer, Rect{0, listTop, pageWidth, listHeight}, count, selectedIndex_,
          [this](int i) {
            const auto& p = inboxPackages_[static_cast<size_t>(i)];
            const auto slash = p.find_last_of('/');
            return slash == std::string::npos ? p : p.substr(slash + 1);
          },
          nullptr, nullptr, nullptr);
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight - 90, "点选安装包后确认");
    }
  }

  const auto labels = mappedInput.mapLabels("« 返回", "确认", "向上", "向下");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
