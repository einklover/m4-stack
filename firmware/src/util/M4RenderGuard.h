#pragma once

// Phase 1 (INV-R1): CrossMux RenderLock port, renamed owner. RAII over the
// process-wide render-submit mutex owned alongside `currentActivity` in
// main.cpp. No policy logic inside the guard: callers obey global-before-local
// ordering with bounded waits only.
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class M4RenderGuard final {
 public:
  explicit M4RenderGuard(SemaphoreHandle_t mutex, TickType_t wait = pdMS_TO_TICKS(100))
      : mutex_(mutex) {
    if (mutex_ != nullptr) {
      owns_ = (xSemaphoreTake(mutex_, wait) == pdTRUE);
    }
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
  [[nodiscard]] static bool peek(SemaphoreHandle_t mutex) {
    if (mutex == nullptr) return false;
    if (xSemaphoreTake(mutex, 0) != pdTRUE) return false;
    xSemaphoreGive(mutex);
    return true;
  }
  [[nodiscard]] bool owns() const { return owns_; }

 private:
  SemaphoreHandle_t mutex_ = nullptr;
  bool owns_ = false;
};
