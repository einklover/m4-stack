#pragma once

// PSRAM-first helpers for native providers.
//
// ESP32-S3 internal RAM is the scarce resource (TLS handshakes need a ~40KB
// contiguous internal block). Payload buffers, HTTP client shells, decode
// windows and FreeRTOS worker stacks should live in PSRAM whenever possible.
//
// Note: mbedTLS still draws its own handshake buffers from internal RAM —
// that cannot be moved without rebuilding Arduino-ESP32. What we *can* do is
// stop competing for internal RAM with everything else.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

#if defined(ARDUINO_ARCH_ESP32)
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#endif

namespace M4Psram {

// TLS needs a large, contiguous internal block. Only small provider metadata
// may use internal RAM, and only while both free and largest-block floors hold.
inline constexpr size_t kLargeInternalFallbackThreshold = 16u * 1024u;
inline constexpr size_t kInternalFreeFloorBytes = 64u * 1024u;
inline constexpr size_t kInternalLargestFloorBytes = 40u * 1024u;

struct AllocationStats {
  uint32_t psramAllocations = 0;
  uint32_t internalFallbacks = 0;
  uint32_t failures = 0;
  uint32_t largeFallbackBlocked = 0;
  uint32_t taskPsram = 0;
  uint32_t taskInternalFallbacks = 0;
  uint32_t taskFailures = 0;
};

inline std::atomic<uint32_t> gPsramAllocations{0};
inline std::atomic<uint32_t> gInternalFallbacks{0};
inline std::atomic<uint32_t> gAllocationFailures{0};
inline std::atomic<uint32_t> gLargeFallbackBlocked{0};
inline std::atomic<uint32_t> gTaskPsram{0};
inline std::atomic<uint32_t> gTaskInternalFallbacks{0};
inline std::atomic<uint32_t> gTaskFailures{0};
#if defined(ARDUINO_ARCH_ESP32)
inline std::mutex gInternalFallbackMutex;
#endif

inline AllocationStats allocationStats() {
  return {gPsramAllocations.load(std::memory_order_relaxed),
          gInternalFallbacks.load(std::memory_order_relaxed),
          gAllocationFailures.load(std::memory_order_relaxed),
          gLargeFallbackBlocked.load(std::memory_order_relaxed),
          gTaskPsram.load(std::memory_order_relaxed),
          gTaskInternalFallbacks.load(std::memory_order_relaxed),
          gTaskFailures.load(std::memory_order_relaxed)};
}

#if defined(ARDUINO_ARCH_ESP32)
inline bool internalFloorAllows(size_t n) {
  const size_t freeBytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return n <= SIZE_MAX - kInternalFreeFloorBytes &&
         n <= SIZE_MAX - kInternalLargestFloorBytes &&
         freeBytes >= n + kInternalFreeFloorBytes &&
         largest >= n + kInternalLargestFloorBytes;
}

inline void logAllocation(const char* purpose, size_t n, const char* result) {
  Serial.printf("[M4-ALLOC] class=%s bytes=%u result=%s int_free=%u int_largest=%u psram_free=%u\n",
                purpose && purpose[0] ? purpose : "provider",
                static_cast<unsigned>(n), result,
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
}
#endif

inline void* mallocPrefer(size_t n, const char* purpose = "provider") {
  if (n == 0) return nullptr;
#if defined(ARDUINO_ARCH_ESP32)
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) {
    gPsramAllocations.fetch_add(1, std::memory_order_relaxed);
    if (n >= kLargeInternalFallbackThreshold) logAllocation(purpose, n, "psram");
    return p;
  }
  if (n >= kLargeInternalFallbackThreshold) {
    gLargeFallbackBlocked.fetch_add(1, std::memory_order_relaxed);
    gAllocationFailures.fetch_add(1, std::memory_order_relaxed);
    logAllocation(purpose, n, "psram_failed_internal_blocked_large");
    return nullptr;
  }
  bool floorAllowed = false;
  {
    std::lock_guard<std::mutex> lock(gInternalFallbackMutex);
    floorAllowed = internalFloorAllows(n);
    if (floorAllowed) p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (!floorAllowed) {
    gAllocationFailures.fetch_add(1, std::memory_order_relaxed);
    logAllocation(purpose, n, "internal_floor_preserved");
    return nullptr;
  }
  if (p) {
    gInternalFallbacks.fetch_add(1, std::memory_order_relaxed);
    logAllocation(purpose, n, "internal_fallback_small");
    return p;
  }
  gAllocationFailures.fetch_add(1, std::memory_order_relaxed);
  logAllocation(purpose, n, "allocation_failed");
  return nullptr;
#else
  return std::malloc(n);
#endif
}

inline void* reallocPrefer(void* ptr, size_t oldSize, size_t n,
                           const char* purpose = "provider-realloc") {
#if defined(ARDUINO_ARCH_ESP32)
  if (!ptr) return mallocPrefer(n, purpose);
  if (n == 0) {
    heap_caps_free(ptr);
    return nullptr;
  }
  void* p = heap_caps_realloc(ptr, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) {
    gPsramAllocations.fetch_add(1, std::memory_order_relaxed);
    if (n >= kLargeInternalFallbackThreshold) logAllocation(purpose, n, "psram_realloc");
    return p;
  }
  // Cross-heap realloc is not always possible; fall back to copy.
  p = mallocPrefer(n, purpose);
  if (!p) return nullptr;
  std::memcpy(p, ptr, oldSize < n ? oldSize : n);
  heap_caps_free(ptr);
  return p;
#else
  (void)oldSize;
  (void)purpose;
  return std::realloc(ptr, n);
#endif
}

inline void freePrefer(void* ptr) {
  if (!ptr) return;
#if defined(ARDUINO_ARCH_ESP32)
  heap_caps_free(ptr);
#else
  std::free(ptr);
#endif
}

// std::unique_ptr that constructs the object in PSRAM (placement new).
template <typename T>
struct Deleter {
  void operator()(T* p) const {
    if (!p) return;
    p->~T();
    freePrefer(p);
  }
};

template <typename T>
using Unique = std::unique_ptr<T, Deleter<T>>;

template <typename T, typename... Args>
Unique<T> makeUnique(Args&&... args) {
  void* mem = mallocPrefer(sizeof(T), "provider-object");
  if (!mem) return Unique<T>(nullptr);
  T* obj = new (mem) T(std::forward<Args>(args)...);
  return Unique<T>(obj);
}

#if defined(ARDUINO_ARCH_ESP32)
// Create worker/display tasks in PSRAM. Large stacks never fall back to
// internal; small stacks may do so only if the internal reserve remains intact.
inline BaseType_t createTask(TaskFunction_t fn, const char* name, uint32_t stackBytes,
                             void* arg, UBaseType_t prio, TaskHandle_t* out) {
  if (out) *out = nullptr;
  BaseType_t ok = xTaskCreateWithCaps(fn, name, stackBytes, arg, prio, out,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (ok == pdPASS) {
    gTaskPsram.fetch_add(1, std::memory_order_relaxed);
    Serial.printf("[M4-TASK] class=%s stack_bytes=%u result=psram\n", name ? name : "provider-task",
                  static_cast<unsigned>(stackBytes));
    return ok;
  }
  const char* purpose = name && name[0] ? name : "provider-task";
  if (stackBytes >= kLargeInternalFallbackThreshold) {
    gTaskFailures.fetch_add(1, std::memory_order_relaxed);
    gLargeFallbackBlocked.fetch_add(1, std::memory_order_relaxed);
    logAllocation(purpose, stackBytes, "psram_stack_failed_internal_blocked");
    return pdFAIL;
  }
  bool floorAllowed = false;
  {
    std::lock_guard<std::mutex> lock(gInternalFallbackMutex);
    floorAllowed = internalFloorAllows(stackBytes);
    if (floorAllowed) {
      if (out) *out = nullptr;
      ok = xTaskCreateWithCaps(fn, name, stackBytes, arg, prio, out,
                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
  }
  if (!floorAllowed) {
    gTaskFailures.fetch_add(1, std::memory_order_relaxed);
    logAllocation(purpose, stackBytes, "psram_stack_failed_internal_floor_preserved");
    return pdFAIL;
  }
  if (ok == pdPASS) {
    gTaskInternalFallbacks.fetch_add(1, std::memory_order_relaxed);
    logAllocation(purpose, stackBytes, "internal_task_fallback_small");
  } else {
    gTaskFailures.fetch_add(1, std::memory_order_relaxed);
    logAllocation(purpose, stackBytes, "task_create_failed");
  }
  return ok;
}

// Pair with createTask. Prefer calling from another task; self-delete with
// nullptr is supported by IDF for WithCaps tasks.
inline void deleteTask(TaskHandle_t handle) { vTaskDeleteWithCaps(handle); }
#endif

inline void logAllocationStats(const char* stage) {
#if defined(ARDUINO_ARCH_ESP32)
  const AllocationStats s = allocationStats();
  Serial.printf("[M4-ALLOC-STATS] stage=%s psram=%u internal_fallback=%u failed=%u large_blocked=%u task_psram=%u task_internal=%u task_failed=%u\n",
                stage ? stage : "?", static_cast<unsigned>(s.psramAllocations),
                static_cast<unsigned>(s.internalFallbacks), static_cast<unsigned>(s.failures),
                static_cast<unsigned>(s.largeFallbackBlocked), static_cast<unsigned>(s.taskPsram),
                static_cast<unsigned>(s.taskInternalFallbacks), static_cast<unsigned>(s.taskFailures));
#else
  (void)stage;
#endif
}

}  // namespace M4Psram
