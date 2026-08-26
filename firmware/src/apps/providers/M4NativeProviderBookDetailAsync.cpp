#include "apps/providers/M4NativeProviderBookDetailAsync.h"

#include "apps/providers/M4Psram.h"
#include "util/M4ProviderCoverCache.h"

#include <Arduino.h>

#include <atomic>
#include <mutex>
#include <utility>

namespace M4NativeProviderBookDetailAsync {
namespace {

std::mutex gMu;
Snapshot gSnapshot;
M4NativeProviderBookDetail::Request gRequest;
int gCoverWidth = 0;
int gCoverHeight = 0;
std::atomic<bool> gBusy{false};
std::atomic<bool> gCancel{false};

bool cancelled() { return gCancel.load(std::memory_order_acquire); }

constexpr uint32_t kCoverSettleMs = 650u;
constexpr uint32_t kCoverSettleSliceMs = 25u;

bool waitForCoverSettle() {
  const uint32_t startedMs = millis();
  while (millis() - startedMs < kCoverSettleMs) {
    if (cancelled()) return false;
    vTaskDelay(pdMS_TO_TICKS(kCoverSettleSliceMs));
  }
  return !cancelled();
}

void publish(Phase phase, M4NativeProviderBookDetail::Result result = {}, std::string coverBmpPath = {}) {
  std::lock_guard<std::mutex> lock(gMu);
  gSnapshot.phase = phase;
  gSnapshot.result = std::move(result);
  gSnapshot.coverBmpPath = std::move(coverBmpPath);
  gSnapshot.updatedMs = millis();
}

void taskMain(void*) {
  M4NativeProviderBookDetail::Request request;
  int coverWidth = 0;
  int coverHeight = 0;
  {
    std::lock_guard<std::mutex> lock(gMu);
    request = gRequest;
    coverWidth = gCoverWidth;
    coverHeight = gCoverHeight;
  }

  auto result = M4NativeProviderBookDetail::fetch(request, cancelled);
  std::string coverBmpPath;
  if (waitForCoverSettle()) {
    auto coverRequest = M4ProviderCoverCache::requestFor(
        request.providerId, request.bookId, result.detail, coverWidth, coverHeight);
    coverRequest.cancelled = cancelled;
    const auto cover = M4ProviderCoverCache::acquireProviderCover(coverRequest);
    if (!cancelled()) coverBmpPath = cover.coverBmpPath;
  }
  publish(result.ok ? Phase::Ready : Phase::Error, std::move(result), std::move(coverBmpPath));
  gBusy.store(false, std::memory_order_release);
  M4Psram::deleteTask(nullptr);
}

}  // namespace

bool start(const M4NativeProviderBookDetail::Request& request,
           int homeCoverWidth, int homeCoverThumbHeight) {
  if (request.providerId.empty() || request.bookId.empty() || request.maxBytes == 0 ||
      homeCoverWidth <= 0 || homeCoverThumbHeight <= 0) {
    return false;
  }
  bool expected = false;
  if (!gBusy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return false;

  gCancel.store(false, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(gMu);
    gRequest = request;
    gCoverWidth = homeCoverWidth;
    gCoverHeight = homeCoverThumbHeight;
    gSnapshot = {};
    gSnapshot.phase = Phase::Loading;
    gSnapshot.providerId = request.providerId;
    gSnapshot.appId = request.appId;
    gSnapshot.bookId = request.bookId;
    gSnapshot.startedMs = millis();
    gSnapshot.updatedMs = gSnapshot.startedMs;
  }

  // Detail JSON is optional, but the HTTP/TLS path still needs the same
  // PSRAM-first stack budget as the provider worker. Priority 0 leaves the
  // UI loop and m4adb responsive while a provider endpoint is slow.
  TaskHandle_t handle = nullptr;
  constexpr uint32_t kDetailStackBytes = 72u * 1024u;
  if (M4Psram::createTask(taskMain, "NativeDetail", kDetailStackBytes, nullptr, 0, &handle) != pdPASS) {
    gBusy.store(false, std::memory_order_release);
    publish(Phase::Error, M4NativeProviderBookDetail::Result{
                              false, M4NativeProviderBookDetail::seed(request), 0, "detail_task_create", false});
    return false;
  }
  (void)handle;
  return true;
}

Snapshot snapshot() {
  std::lock_guard<std::mutex> lock(gMu);
  return gSnapshot;
}

bool busy() { return gBusy.load(std::memory_order_acquire); }

void cancel() { gCancel.store(true, std::memory_order_release); }

}  // namespace M4NativeProviderBookDetailAsync
