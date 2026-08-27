#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <string>

#include "activities/ActivityWithSubactivity.h"
#include "network/OtaManager.h"

/**
 * Online OTA update activity.
 * Checks for firmware updates from the server, downloads, verifies MD5,
 * and flashes using the existing SdOtaUpdater pipeline.
 */
class OnlineOtaActivity : public ActivityWithSubactivity {
  enum State {
    CHECKING_WIFI,
    WIFI_PROMPT,
    CHECKING_VERSION,
    NO_UPDATE,
    UPDATE_AVAILABLE,
    CHECK_FAILED,
    DOWNLOADING,
    DOWNLOAD_FAILED,
    VERIFYING_MD5,
    MD5_FAILED,
    READY_TO_FLASH,
    FLASHING,
    FLASH_FAILED,
    FINISHED,
    SHUTTING_DOWN,
  };

  static constexpr unsigned int UNINITIALIZED_PERCENTAGE = 111;

  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;
  const std::function<void()> goBack;

  State state = CHECKING_WIFI;
  OtaManager otaManager;

  // 下载进度
  size_t downloadedBytes = 0;
  size_t totalBytes = 0;
  unsigned int lastPercentage = UNINITIALIZED_PERCENTAGE;

  // 刷机进度
  size_t flashDone = 0;
  size_t flashTotal = 0;

  std::string errorMessage;
  
  // 滚动状态
  int remarkScrollOffset = 0;  // 备注文本滚动偏移
  int remarkMaxLines = 0;      // 备注文本最大可见行数

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render();

  void startVersionCheck();
  void startDownload();
  void startFlash();

 public:
  explicit OnlineOtaActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                              const std::function<void()>& goBack)
      : ActivityWithSubactivity("OnlineOta", renderer, mappedInput), goBack(goBack) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  uint8_t touchFooterButtonsMask() const override {
    return M4FooterTouchPolicy::Back | M4FooterTouchPolicy::Confirm;
  }
  bool preventAutoSleep() override { return state == FLASHING || state == DOWNLOADING; }
};
