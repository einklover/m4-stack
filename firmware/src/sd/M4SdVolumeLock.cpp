#include "M4SdVolumeGuard.h"

#if defined(M4_SD_VOLUME_LOCK_HOST)

#include <mutex>

namespace {

std::recursive_mutex gMu;
int gDepth = 0;

}  // namespace

M4SdVolumeGuard::M4SdVolumeGuard() {
  gMu.lock();
  ++gDepth;
}

M4SdVolumeGuard::~M4SdVolumeGuard() {
  --gDepth;
  gMu.unlock();
}

bool m4SdVolumeLockTry() {
  if (!gMu.try_lock()) return false;
  ++gDepth;
  return true;
}

void m4SdVolumeLockGive() {
  --gDepth;
  gMu.unlock();
}

int m4SdVolumeLockDepth() { return gDepth; }

#else

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {

portMUX_TYPE gInitMux = portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t gMu = nullptr;
int gDepth = 0;

void ensureMutex() {
  if (gMu) return;
  portENTER_CRITICAL(&gInitMux);
  if (!gMu) gMu = xSemaphoreCreateRecursiveMutex();
  portEXIT_CRITICAL(&gInitMux);
}

}  // namespace

M4SdVolumeGuard::M4SdVolumeGuard() {
  ensureMutex();
  // Fail closed: do not touch the shared cache without the mutex.
  while (!gMu || xSemaphoreTakeRecursive(gMu, portMAX_DELAY) != pdTRUE) {
  }
  ++gDepth;
}

M4SdVolumeGuard::~M4SdVolumeGuard() {
  --gDepth;
  xSemaphoreGiveRecursive(gMu);
}

bool m4SdVolumeLockTry() {
  ensureMutex();
  if (!gMu || xSemaphoreTakeRecursive(gMu, 0) != pdTRUE) return false;
  ++gDepth;
  return true;
}

void m4SdVolumeLockGive() {
  --gDepth;
  xSemaphoreGiveRecursive(gMu);
}

int m4SdVolumeLockDepth() { return gDepth; }

#endif
