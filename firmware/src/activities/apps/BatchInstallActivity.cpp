#include "BatchInstallActivity.h"

#include <GfxRenderer.h>
#include <SDCardManager.h>
#include <esp_task_wdt.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include "MappedInputManager.h"
#include "apps/M4xInboxBatch.h"
#include "apps/M4xInstaller.h"
#include "apps/M4xPaths.h"
#include "apps/M4xRegistry.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"

struct BatchInstallActivity::Job {
  std::atomic<int> total{0};
  std::atomic<int> processed{0};
  std::atomic<int> installed{0};
  std::atomic<int> skipped{0};
  std::atomic<int> failed{0};
  std::atomic<bool> scanning{true};
  std::atomic<bool> done{false};
};

namespace {

bool isM4x(const char* name) {
  if (!name) return false;
  std::string s(name);
  if (s.size() < 4) return false;
  const size_t p = s.size() - 4;
  return s[p] == '.' && (s[p + 1] == 'm' || s[p + 1] == 'M') && s[p + 2] == '4' &&
         (s[p + 3] == 'x' || s[p + 3] == 'X');
}

std::vector<std::string> scanInbox() {
  std::vector<std::string> paths;
  M4xInstaller::ensureLayout();
  FsFile root = SdMan.open(M4xPaths::kInbox);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return paths;
  }
  FsFile file = root.openNextFile();
  char name[256] = {};
  while (file) {
    file.getName(name, sizeof(name));
    if (!file.isDirectory() && isM4x(name)) {
      std::string path = name;
      if (path.empty() || path[0] != '/') path = std::string(M4xPaths::kInbox) + "/" + path;
      paths.push_back(std::move(path));
    }
    file.close();
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(1));
    file = root.openNextFile();
  }
  root.close();
  return paths;
}

void batchTaskTrampoline(void* param) {
  auto* job = static_cast<BatchInstallActivity::Job*>(param);
  // Directory enumeration is part of the worker too. A large /apps_inbox
  // must not block the first paint or touch polling on the UI task.
  const auto paths = scanInbox();
  job->total.store(static_cast<int>(paths.size()), std::memory_order_release);
  job->scanning.store(false, std::memory_order_release);
  for (const auto& path : paths) {
    const auto probe = M4xInstaller::probe(path);
    if (!probe.ok) {
      Serial.printf("[M4x] batch probe failed: %s (%s)\n", path.c_str(), probe.error.c_str());
      job->failed.fetch_add(1, std::memory_order_relaxed);
      job->processed.fetch_add(1, std::memory_order_release);
      continue;
    }

    const auto apps = M4xRegistry::load();
    const auto* installed = M4xRegistry::find(apps, probe.manifest.id);
    const auto decision = m4xInboxBatchDecide(
        true, installed != nullptr, probe.manifest.versionCode, installed ? installed->versionCode : 0);
    if (decision == M4xInboxBatchAction::SkipDelete) {
      if (SdMan.remove(path.c_str())) {
        job->skipped.fetch_add(1, std::memory_order_relaxed);
      } else {
        job->failed.fetch_add(1, std::memory_order_relaxed);
      }
      job->processed.fetch_add(1, std::memory_order_release);
      continue;
    }

    const auto result = M4xInstaller::install(path);
    if (!result.ok) {
      Serial.printf("[M4x] batch install failed: %s (%s)\n", path.c_str(), result.error.c_str());
      job->failed.fetch_add(1, std::memory_order_relaxed);
    } else if (SdMan.remove(path.c_str())) {
      job->installed.fetch_add(1, std::memory_order_relaxed);
    } else {
      // Installation is complete, but keep the package when cleanup fails so
      // the user can remove/retry it; report the cleanup failure explicitly.
      job->failed.fetch_add(1, std::memory_order_relaxed);
    }
    job->processed.fetch_add(1, std::memory_order_release);
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  job->done.store(true, std::memory_order_release);
  vTaskDelete(nullptr);
}

}  // namespace

void BatchInstallActivity::displayTaskTrampoline(void* param) {
  static_cast<BatchInstallActivity*>(param)->displayTaskLoop();
}

void BatchInstallActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  renderingMutex_ = xSemaphoreCreateMutex();
  job_ = new Job();
  const BaseType_t ok = xTaskCreate(batchTaskTrampoline, "M4xBatch", 12288, job_, 1, nullptr);
  if (ok != pdPASS) {
    job_->scanning.store(false, std::memory_order_release);
    job_->failed.store(1, std::memory_order_relaxed);
    job_->done.store(true, std::memory_order_release);
  }
  updateRequired_ = true;
  xTaskCreate(&BatchInstallActivity::displayTaskTrampoline, "M4xBatchUI", 4096, this, 1,
              &displayTaskHandle_);
}

void BatchInstallActivity::onExit() {
  ActivityWithSubactivity::onExit();
  if (job_ && !job_->done.load(std::memory_order_acquire)) {
    // The worker owns the SD/ZIP stack. Wait for its real completion instead
    // of destroying the job after a timeout and leaving a use-after-free task.
    while (!job_->done.load(std::memory_order_acquire)) {
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }
  if (renderingMutex_) {
    xSemaphoreTake(renderingMutex_, portMAX_DELAY);
    if (displayTaskHandle_) {
      vTaskDelete(displayTaskHandle_);
      displayTaskHandle_ = nullptr;
    }
    xSemaphoreGive(renderingMutex_);
    vSemaphoreDelete(renderingMutex_);
    renderingMutex_ = nullptr;
  }
  if (job_ && job_->done.load(std::memory_order_acquire)) {
    delete job_;
  }
  job_ = nullptr;
}

void BatchInstallActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired_) {
      updateRequired_ = false;
      xSemaphoreTake(renderingMutex_, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex_);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void BatchInstallActivity::loop() {
  if (!job_) return;
  const bool done = job_->done.load(std::memory_order_acquire);
  if (!done) {
    updateRequired_ = true;
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasBackGesture()) {
    onDone_();
    return;
  }
  int x = 0, y = 0;
  if (mappedInput.hasTouch() && mappedInput.wasScreenTapped(x, y)) {
    onDone_();
  }
}

void BatchInstallActivity::render() const {
  renderer.clearScreen();
  const auto metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 "批量安装插件");

  const int total = job_ ? job_->total.load(std::memory_order_acquire) : 0;
  const int processed = job_ ? job_->processed.load(std::memory_order_acquire) : 0;
  const int installed = job_ ? job_->installed.load(std::memory_order_relaxed) : 0;
  const int skipped = job_ ? job_->skipped.load(std::memory_order_relaxed) : 0;
  const int failed = job_ ? job_->failed.load(std::memory_order_relaxed) : 0;
  if (job_ && !job_->done.load(std::memory_order_acquire)) {
    char line[96];
    if (job_->scanning.load(std::memory_order_acquire)) {
      std::snprintf(line, sizeof(line), "正在扫描 apps_inbox…");
    } else {
      std::snprintf(line, sizeof(line), "正在处理 %d / %d", processed, total);
    }
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, 270, line, true, EpdFontFamily::BOLD);
  } else {
    char line[160];
    std::snprintf(line, sizeof(line), "安装 %d，跳过 %d，失败 %d", installed, skipped, failed);
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, 250, line, true, EpdFontFamily::BOLD);
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 310, "已完成；点按屏幕或返回键退出");
    const int w = std::min(360, renderer.getScreenWidth() - 40);
    const int h = 64;
    const int x = (renderer.getScreenWidth() - w) / 2;
    const int y = renderer.getScreenHeight() - 150;
    renderer.fillRoundedRect(x, y, w, h, 12, Color::Black);
    M4UiText::drawCenteredInBox(renderer, UI_12_FONT_ID, x, y, w, h, "完成", false, EpdFontFamily::BOLD,
                                8);
  }
  renderer.displayBuffer();
}
