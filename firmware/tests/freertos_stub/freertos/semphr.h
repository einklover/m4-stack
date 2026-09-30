#pragma once
// Owner-aware recursive mutex stand-in for the production lock branch.
#include <mutex>
#include <thread>

typedef void* SemaphoreHandle_t;
typedef int BaseType_t;
typedef struct { int unused; } StaticSemaphore_t;

#define portMAX_DELAY 0xffffffffu
#define pdTRUE 1
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))

inline std::recursive_mutex gStubMu;
inline int gStubTakes = 0;
inline int gStubCreates = 0;
inline std::thread::id gStubOwner;
inline int gStubOwnerDepth = 0;

inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t*) {
  ++gStubCreates;
  return reinterpret_cast<SemaphoreHandle_t>(1);
}

inline BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t handle, uint32_t ticks) {
  if (!handle) return 0;
  ++gStubTakes;
  if (ticks == 0) {
    if (!gStubMu.try_lock()) return 0;
  } else {
    gStubMu.lock();
  }
  if (gStubOwnerDepth == 0) gStubOwner = std::this_thread::get_id();
  ++gStubOwnerDepth;
  return pdTRUE;
}

inline BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t handle) {
  if (!handle) return 0;
  if (gStubOwner != std::this_thread::get_id() || gStubOwnerDepth <= 0) return 0;
  --gStubOwnerDepth;
  gStubMu.unlock();
  return pdTRUE;
}
