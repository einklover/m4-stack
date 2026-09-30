#include "BatchInstallActivity.h"

#include <GfxRenderer.h>
#include <SDCardManager.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>

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

bool isM4xName(const char* name, size_t length) {
  if (!name || length < 4) return false;
  const size_t p = length - 4;
  return name[p] == '.' && (name[p + 1] == 'm' || name[p + 1] == 'M') && name[p + 2] == '4' &&
         (name[p + 3] == 'x' || name[p + 3] == 'X');
}

void processInboxPackage(BatchInstallActivity::Job* job, const std::string& path) {
  const auto probe = M4xInstaller::probe(path);
  if (!probe.ok) {
    Serial.printf("[M4x] batch probe failed: %s (%s)\n", path.c_str(), probe.error.c_str());
    job->failed.fetch_add(1, std::memory_order_relaxed);
    job->processed.fetch_add(1, std::memory_order_release);
    return;
  }

  // The registry vector dies before install or remove, and before done.
  M4xInboxBatchAction decision = M4xInboxBatchAction::Install;
  {
    const auto apps = M4xRegistry::load();
    const auto* installed = M4xRegistry::find(apps, probe.manifest.id);
    decision = m4xInboxBatchDecide(true, installed != nullptr, probe.manifest.versionCode,
                                   installed ? installed->versionCode : 0);
  }
  if (decision == M4xInboxBatchAction::SkipDelete) {
    if (SdMan.remove(path.c_str())) {
      job->skipped.fetch_add(1, std::memory_order_relaxed);
    } else {
      job->failed.fetch_add(1, std::memory_order_relaxed);
    }
    job->processed.fetch_add(1, std::memory_order_release);
    return;
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
}

// One parent cursor for the whole inbox. The child is closed before probe,
// install, or remove. SdFat remove does not rewind that parent, so deleting
// the current entry neither skips nor repeats the next sibling.
// A name failure or a root read error increments failed and is not a
// discovered package, so installed + skipped + failed can exceed total.
void runInboxBatch(BatchInstallActivity::Job* job) {
  // FAT LFN is 255 UTF-16 units. A BMP code point is at most 3 UTF-8 bytes.
  constexpr size_t kInboxNameBytes = 255 * 3 + 1;
  M4xInstaller::ensureLayout();
  FsFile root = SdMan.open(M4xPaths::kInbox);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    job->scanning.store(false, std::memory_order_release);
    job->failed.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  job->scanning.store(false, std::memory_order_release);
  while (true) {
    char name[kInboxNameBytes];
    size_t nameLen = 0;
    bool directory = false;
    {
      FsFile child = root.openNextFile();
      if (!child) {
        if (root.getError()) job->failed.fetch_add(1, std::memory_order_relaxed);
        break;
      }
      nameLen = child.getName(name, sizeof(name));
      directory = child.isDirectory();
      child.close();
    }
    if (nameLen == 0 || nameLen >= sizeof(name) || name[nameLen] != '\0') {
      if (!directory) job->failed.fetch_add(1, std::memory_order_relaxed);
      esp_task_wdt_reset();
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }
    if (directory || !isM4xName(name, nameLen)) {
      esp_task_wdt_reset();
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }
    job->total.fetch_add(1, std::memory_order_release);
    {
      std::string path(name, nameLen);
      if (path.empty() || path[0] != '/') path = std::string(M4xPaths::kInbox) + "/" + path;
      processInboxPackage(job, path);
    }
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  root.close();
}

void batchTaskTrampoline(void* param) {
  auto* job = static_cast<BatchInstallActivity::Job*>(param);
  runInboxBatch(job);
  job->done.store(true, std::memory_order_release);
  vTaskDelete(nullptr);
}

}  // namespace

void BatchInstallActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  hasPaint_ = false;
  lastPaintMs_ = 0;
  painted_ = {};
  job_ = new Job();
  const BaseType_t ok = xTaskCreate(batchTaskTrampoline, "M4xBatch", 12288, job_, 1, nullptr);
  if (ok != pdPASS) {
    job_->scanning.store(false, std::memory_order_release);
    job_->failed.store(1, std::memory_order_relaxed);
    job_->done.store(true, std::memory_order_release);
  }
}

void BatchInstallActivity::onExit() {
  ActivityWithSubactivity::onExit();
  // The worker still owns the install transaction. There is no process-wide
  // install mutex yet, so this screen waits until job_->done. Leaving early
  // would allow another install to start. That wait belongs to the later
  // unified install-ownership fix.
  if (job_ && !job_->done.load(std::memory_order_acquire)) {
    while (!job_->done.load(std::memory_order_acquire)) {
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }
  if (job_ && job_->done.load(std::memory_order_acquire)) {
    delete job_;
  }
  job_ = nullptr;
}

void BatchInstallActivity::loop() {
  if (!job_) return;
  const unsigned long nowMs = millis();
  constexpr unsigned long kIntervalMs = 500;
  ProgressView now;
  now.total = job_->total.load(std::memory_order_acquire);
  now.processed = job_->processed.load(std::memory_order_acquire);
  now.installed = job_->installed.load(std::memory_order_relaxed);
  now.skipped = job_->skipped.load(std::memory_order_relaxed);
  now.failed = job_->failed.load(std::memory_order_relaxed);
  now.scanning = job_->scanning.load(std::memory_order_acquire);
  now.done = job_->done.load(std::memory_order_acquire);
  const bool changed = !hasPaint_ || now.total != painted_.total || now.processed != painted_.processed ||
                       now.installed != painted_.installed || now.skipped != painted_.skipped ||
                       now.failed != painted_.failed || now.scanning != painted_.scanning ||
                       now.done != painted_.done;
  const bool due = !hasPaint_ || now.done || (nowMs - lastPaintMs_) >= kIntervalMs;
  if (changed && due) {
    painted_ = now;
    hasPaint_ = true;
    lastPaintMs_ = nowMs;
    render();
  }
  if (!now.done) return;
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
      std::snprintf(line, sizeof(line), "已处理 %d，已发现 %d", processed, total);
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
