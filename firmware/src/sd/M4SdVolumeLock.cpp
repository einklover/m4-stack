#include "M4SdVolumeGuard.h"

#if defined(M4_SD_VOLUME_LOCK_HOST)

#include <mutex>

namespace {

std::recursive_mutex gMu;
int gDepth = 0;
int (*gCreateHook)() = nullptr;

}  // namespace

void m4SdVolumeLockSetCreateHook(int (*hook)()) { gCreateHook = hook; }

bool m4SdVolumeLockPrepare() {
  if (gCreateHook && gCreateHook()) return false;
  return true;
}

M4SdVolumeGuard::M4SdVolumeGuard() {
  if (!m4SdVolumeLockPrepare()) return;
  gMu.lock();
  held_ = true;
  ++gDepth;
}

M4SdVolumeGuard::~M4SdVolumeGuard() {
  if (!held_) return;
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
#include <stdlib.h>

namespace {

portMUX_TYPE gInitMux = portMUX_INITIALIZER_UNLOCKED;
StaticSemaphore_t gMuStorage;
SemaphoreHandle_t gMu = nullptr;
int gDepth = 0;
int (*gCreateHook)() = nullptr;

void ensureMutex() {
  if (gMu) return;
  portENTER_CRITICAL(&gInitMux);
  if (!gMu) {
    if (gCreateHook && gCreateHook()) {
      portEXIT_CRITICAL(&gInitMux);
      return;
    }
    // Static storage: a failed heap alloc cannot leave this null on the
    // production path. The hook above is the test reject.
    gMu = xSemaphoreCreateRecursiveMutexStatic(&gMuStorage);
  }
  portEXIT_CRITICAL(&gInitMux);
}

}  // namespace

void m4SdVolumeLockSetCreateHook(int (*hook)()) { gCreateHook = hook; }

bool m4SdVolumeLockPrepare() {
  ensureMutex();
  return gMu != nullptr;
}

M4SdVolumeGuard::M4SdVolumeGuard() {
  // Create failure returns. Callers must have seen Prepare() == false and
  // skipped the cache. Do not spin, and do not take a null mutex.
  if (!m4SdVolumeLockPrepare()) return;
  // Create already succeeded. A failed take must not fall through into the cache.
  if (xSemaphoreTakeRecursive(gMu, portMAX_DELAY) != pdTRUE) abort();
  held_ = true;
  ++gDepth;
}

M4SdVolumeGuard::~M4SdVolumeGuard() {
  if (!held_) return;
  --gDepth;
  xSemaphoreGiveRecursive(gMu);
}

bool m4SdVolumeLockTry() {
  if (!m4SdVolumeLockPrepare()) return false;
  if (xSemaphoreTakeRecursive(gMu, 0) != pdTRUE) return false;
  ++gDepth;
  return true;
}

void m4SdVolumeLockGive() {
  if (gDepth <= 0 || !gMu) return;
  --gDepth;
  xSemaphoreGiveRecursive(gMu);
}

int m4SdVolumeLockDepth() { return gDepth; }

#endif
