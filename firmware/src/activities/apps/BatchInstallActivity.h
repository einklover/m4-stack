#pragma once

#include "../ActivityWithSubactivity.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>

// Installs every .m4x currently in /apps_inbox. Invalid or failed packages are
// deliberately left in place for inspection/retry; successful and obsolete
// packages are removed by the worker.
class BatchInstallActivity final : public ActivityWithSubactivity {
 public:
  BatchInstallActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                       const std::function<void()>& onDone)
      : ActivityWithSubactivity("BatchInstall", renderer, mappedInput), onDone_(onDone) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

  uint8_t touchFooterButtonsMask() const override {
    return M4FooterTouchPolicy::Back | M4FooterTouchPolicy::Confirm;
  }

 public:
  struct Job;

 private:

  const std::function<void()> onDone_;
  Job* job_ = nullptr;
  TaskHandle_t displayTaskHandle_ = nullptr;
  SemaphoreHandle_t renderingMutex_ = nullptr;
  bool updateRequired_ = false;

  static void displayTaskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;
};
