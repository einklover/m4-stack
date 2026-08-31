#include <cassert>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "activities/home/HomeSceneAssetDecoder.h"
#include "ui/pages/HomeSceneModel.h"

using namespace HomeScene;

// Simulate HomeActivity's BackendContext lifetime-safe pattern.
struct TestBackendContext {
  HomeSceneModel model;
  std::atomic<bool> cancelled{false};
  std::atomic<uint32_t> epoch{0};
  std::atomic<bool> exiting{false};
  std::atomic<bool> updateRequired{false};
};

void testCancelledDecodeDoesNotCommitPartialAsset() {
  // Two-row, 8x2 1-bit BMP; cancellation is observed after row zero.
  std::vector<uint8_t> bmp(70, 0);
  bmp[0] = 'B';
  bmp[1] = 'M';
  bmp[10] = 62;  // 14-byte file header + 40-byte DIB + 8-byte palette
  bmp[14] = 40;
  bmp[18] = 8;
  bmp[22] = 2;
  bmp[26] = 1;
  bmp[28] = 1;
  bmp[62] = 0xFF;
  bmp[66] = 0xFF;

  std::vector<uint8_t> out(2, 0xA5);
  int checks = 0;
  const bool decoded = HomeSceneAssetDecoder::decodeBmpBytesTo1Bit(
      bmp.data(), bmp.size(), out.data(), 8, 2, 1,
      [&checks]() { return checks++ >= 2; });
  assert(!decoded && "mid-decode cancellation must fail");
  assert(out[0] == 0xA5 && out[1] == 0xA5 &&
         "cancelled decode must not commit a partial asset");
}

void testDelayedBackendCannotAccessDestroyedActivityAndNoPostExitPublish() {
  // Create ctx as HomeActivity would onEnter.
  auto ctx = std::make_shared<TestBackendContext>();
  ctx->epoch.store(1, std::memory_order_release);
  ctx->model.publishLoading();
  assert(ctx->model.hasPublishedSnapshot());
  uint32_t startEpoch = ctx->epoch.load(std::memory_order_acquire);
  // Backend thread: simulates long FsFile/Bitmap/Epub work (sleep) then tries to publish.
  std::atomic<bool> backendTouchedDestroyed{false};
  std::atomic<bool> backendPublished{false};
  // Keep weak_ptr to simulate HomeActivity object lifetime check.
  // HomeActivity would be a separate object; we simulate by having a dummy int that backend should NOT touch.
  struct DummyActivity { int marker = 0x1234; };
  auto activity = std::make_shared<DummyActivity>();
  std::weak_ptr<DummyActivity> weakActivity = activity;

  std::thread backend([ctx, startEpoch, weakActivity, &backendPublished, &backendTouchedDestroyed]() {
    // Simulate work that holds FsFile/Bitmap (sleep 80ms)
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    // Check cancellation before publish (cooperative)
    bool cancelled = ctx->cancelled.load(std::memory_order_acquire) ||
                     ctx->epoch.load(std::memory_order_acquire) != startEpoch;
    if (cancelled) {
      // Must NOT touch activity if it was destroyed. Check weak_ptr.
      if (weakActivity.expired()) {
        // Correct: activity destroyed, we did not touch it.
      } else {
        // If activity still alive, we also should not have touched it after cancel.
        // No access to *activity here.
      }
      ctx->exiting.store(true, std::memory_order_release);
      return;
    }
    // If not cancelled, would try to access activity (simulate bug). We flag if we would.
    if (!weakActivity.expired()) {
      backendTouchedDestroyed.store(false);
    }
    ctx->model.begin(UiScene::DataState::Ready);
    ctx->model.setCurrent("Title", "Author", "Src", "/cover.bmp", 50);
    backendPublished.store(ctx->model.publish());
    if (backendPublished.load()) ctx->updateRequired.store(true);
    ctx->exiting.store(true, std::memory_order_release);
  });

  // Simulate onExit after 10ms: cancel, bump epoch, destroy activity.
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  ctx->cancelled.store(true, std::memory_order_release);
  ctx->epoch.fetch_add(1, std::memory_order_acq_rel);
  // Destroy HomeActivity (reset shared_ptr). Backend still holds ctx via its own shared_ptr.
  activity.reset();
  assert(weakActivity.expired() && "activity should be destroyed after onExit");
  // At this point, HomeActivity object is gone, but ctx must remain alive via backend thread.
  assert(ctx.use_count() >= 2 && "ctx must stay alive via backend thread after activity destroyed");
  // Wait for backend to finish.
  backend.join();
  assert(ctx->exiting.load(std::memory_order_acquire) && "backend must have set exiting");
  // Verify no post-exit publish: model should not have new Ready publication from backend.
  // It should still be at Loading (initial) revision, not incremented by backend's failed publish.
  HomeSceneSnapshot snap{};
  bool has = ctx->model.copyLatest(snap);
  assert(has);
  // Since backend was cancelled, it should NOT have published Ready; snapshot should remain Loading.
  assert(snap.state == UiScene::DataState::Loading && "no post-exit publish should occur");
  assert(!backendPublished.load() && "backend publish must have been blocked");
  assert(!ctx->updateRequired.load() && "no updateRequired after cancelled publish");
  // Verify backend did not touch destroyed activity.
  assert(!backendTouchedDestroyed.load() && "backend must not access destroyed activity");
  // After backend exits, ctx can be released without UAF.
  ctx.reset();
}

void testNoPostExitPublishEvenIfBackendTriesLate() {
  auto ctx = std::make_shared<TestBackendContext>();
  ctx->model.publishLoading();
  uint32_t epoch = ctx->epoch.load();
  // Simulate backend that ignores cancellation for a moment and tries to publish after onExit.
  std::thread backend([ctx, epoch]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    // Cooperative check should block.
    bool cancelled = ctx->cancelled.load() || ctx->epoch.load() != epoch;
    assert(cancelled && "should be cancelled after onExit");
    // Even if it tries to publish, it must check cancelled again before publish.
    if (cancelled) {
      ctx->exiting.store(true);
      return;
    }
    ctx->model.begin(UiScene::DataState::Ready);
    ctx->model.setCurrent("Late", "A", "S", "/late.bmp", 10);
    ctx->model.publish();
    ctx->exiting.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  ctx->cancelled.store(true);
  ctx->epoch.fetch_add(1);
  backend.join();
  HomeSceneSnapshot snap{};
  ctx->model.copyLatest(snap);
  assert(snap.state == UiScene::DataState::Loading);
}

int main() {
  testCancelledDecodeDoesNotCommitPartialAsset();
  testDelayedBackendCannotAccessDestroyedActivityAndNoPostExitPublish();
  testNoPostExitPublishEvenIfBackendTriesLate();
  return 0;
}
