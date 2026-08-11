#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "activities/ActivityWithSubactivity.h"
#include "network/SdOtaUpdater.h"

/**
 * SD card OTA update activity.
 * Expects firmware.bin at /update/firmware.bin on SD card.
 * No WiFi needed — just reads from SD and flashes.
 */
class SdOtaUpdateActivity : public ActivityWithSubactivity {
  enum State {
    CHECKING_SD,
    WAITING_CONFIRMATION,
    FLASHING,
    NO_FIRMWARE,
    FAILED,
    FINISHED,
    SHUTTING_DOWN
  };

  static constexpr unsigned int UNINITIALIZED_PERCENTAGE = 111;

  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;
  const std::function<void()> goBack;
  State state = CHECKING_SD;
  unsigned int lastPercentage = UNINITIALIZED_PERCENTAGE;
  size_t progressDone = 0;
  size_t progressTotal = 0;
  SdOtaUpdater updater;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render();

 public:
  explicit SdOtaUpdateActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                const std::function<void()>& goBack)
      : ActivityWithSubactivity("SdOtaUpdate", renderer, mappedInput), goBack(goBack) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool preventAutoSleep() override { return state == FLASHING; }
};
