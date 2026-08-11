#pragma once

// Fullscreen reader for provider "screenbridge": fetches pages from an Android
// accessibility bridge on fixed LAN port 48624.
//
// Wire protocol (see util/M4ScreenFrameCodec.h and docs/SCREEN_BRIDGE_PROTOCOL.md):
//   GET  /v1/status              -> JSON status
//   GET  /v1/page?index=N        -> 24-byte header + payload (raw1 / RLE1)
//   POST /v1/consume?index=N     -> mark page N consumed
// Physical frame is 800x480x1, MSB first, 1=white. Matches HalDisplay memory,
// so a validated 48000-byte frame is memcpy'd straight into the renderer
// framebuffer and flushed with FAST_REFRESH.
//
// Networking/decode/CRC run on a PSRAM worker task that fills a small ring
// cache (current/next/previous). Input handling never touches the network: it
// switches instantly to a cached frame and only records intent for the worker.

#include "../Activity.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

class ScreenBridgeActivity final : public Activity {
 public:
  ScreenBridgeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string appId,
                       const std::function<void()>& onExit);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  std::string debugUiJson() override;

  bool isFullscreenActivity() const override { return true; }
  bool isReaderBodyActivity() const override { return true; }

 private:
  // pickTargetLocked keeps current, +/-1 and +/-2 warm. Fewer slots make the
  // worker evict and refetch forever, which also starves input between refreshes.
  static constexpr int kSlots = 5;
  static_assert(kSlots >= 5, "screen bridge cache must hold the prefetch window");
  static constexpr size_t kFrameBytes = 48000;

  enum class Phase : uint8_t { Idle = 0, Discovering, Connecting, Ready, Error };

  struct CacheSlot {
    int32_t page = -1;
    uint8_t* data = nullptr;
    bool valid = false;
    uint32_t seq = 0;
  };

  static void taskTrampoline(void* param);
  void taskLoop();

  void startWorker();
  void stopWorker();
  bool stopped() const;

  // Worker side.
  bool discoverEndpoint();
  bool probeStatus(const std::string& base);
  void adopt(const std::string& base);
  bool fetchAndStore(const std::string& base, int32_t page);
  void sendConsume(const std::string& base, int32_t page);
  bool sendTap(const std::string& base, int x, int y);
  void setPhase(Phase phase, const std::string& error);
  int32_t pickTargetLocked();           // caller holds mu_
  bool slotCached(int32_t page) const;  // caller holds mu_
  CacheSlot* slotFor(int32_t page);     // caller holds mu_
  CacheSlot* freeSlot();                // caller holds mu_

  // Main side.
  void handlePrev();
  void handleNext();
  bool queueRealtimeTap(int x, int y);
  void renderNow();
  void renderStatusScreen(Phase phase, const std::string& base, const std::string& error);
  void persistEndpoint(const std::string& base);

  std::string appId_;
  std::string appDataRoot_;
  std::string endpointPath_;
  std::function<void()> onExit_;

  // Shared with worker.
  mutable std::mutex mu_;
  CacheSlot slots_[kSlots];
  uint32_t nextSeq_ = 0;
  int32_t current_ = 0;
  int32_t pendingTarget_ = -1;
  int32_t consumePending_ = -1;
  int32_t maxPage_ = -1;
  Phase phase_ = Phase::Idle;
  std::string base_;
  std::string lastIp_;
  std::string error_;
  bool cacheEnabled_ = true;
  int tapPendingX_ = -1;
  int tapPendingY_ = -1;
  uint32_t paintSig_ = 0;

#if defined(ARDUINO_ARCH_ESP32)
  TaskHandle_t task_ = nullptr;
#endif
  bool stop_ = false;  // written by main, read by worker under mu_

  // Main-thread only.
  uint32_t lastPaintSig_ = 0;
  uint32_t ignoreTouchUntilMs_ = 0;
};
