#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Process-wide render-submit guard. Activities use the same global-before-local
// lock order so a parent display task cannot submit over a child first frame.
class M4RenderGuard final {
 public:
  explicit M4RenderGuard(SemaphoreHandle_t mutex, TickType_t wait = pdMS_TO_TICKS(100))
      : mutex_(mutex) {
    if (mutex_ != nullptr) owns_ = (xSemaphoreTake(mutex_, wait) == pdTRUE);
  }
  ~M4RenderGuard() { unlock(); }
  M4RenderGuard(const M4RenderGuard&) = delete;
  M4RenderGuard& operator=(const M4RenderGuard&) = delete;
  M4RenderGuard(M4RenderGuard&&) = delete;
  M4RenderGuard& operator=(M4RenderGuard&&) = delete;

  void unlock() {
    if (owns_) {
      xSemaphoreGive(mutex_);
      owns_ = false;
    }
  }

  [[nodiscard]] bool owns() const { return owns_; }

 private:
  SemaphoreHandle_t mutex_ = nullptr;
  bool owns_ = false;
};
