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
#include <memory>
#include <new>
#include <utility>

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#endif

namespace M4Psram {

inline void* mallocPrefer(size_t n) {
  if (n == 0) return nullptr;
#if defined(ARDUINO_ARCH_ESP32)
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) return p;
  return heap_caps_malloc(n, MALLOC_CAP_8BIT);
#else
  return std::malloc(n);
#endif
}

inline void* reallocPrefer(void* ptr, size_t n) {
#if defined(ARDUINO_ARCH_ESP32)
  if (!ptr) return mallocPrefer(n);
  if (n == 0) {
    heap_caps_free(ptr);
    return nullptr;
  }
  void* p = heap_caps_realloc(ptr, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) return p;
  // Cross-heap realloc is not always possible; fall back to copy.
  p = mallocPrefer(n);
  if (!p) return nullptr;
  // Best-effort: size of previous block is unknown; callers that need safe
  // growth should track size. For buffers we only grow, so this is unused.
  heap_caps_free(ptr);
  return p;
#else
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
  void* mem = mallocPrefer(sizeof(T));
  if (!mem) return Unique<T>(nullptr);
  T* obj = new (mem) T(std::forward<Args>(args)...);
  return Unique<T>(obj);
}

#if defined(ARDUINO_ARCH_ESP32)
// Create a FreeRTOS task whose *stack* is allocated from PSRAM so the large
// NativeProvider worker (40–48KB) does not steal the contiguous internal
// block that mbedTLS needs for TLS. Falls back to an internal-caps stack
// (still via WithCaps so deleteTask is uniform) if PSRAM is exhausted.
inline BaseType_t createTask(TaskFunction_t fn, const char* name, uint32_t stackBytes,
                             void* arg, UBaseType_t prio, TaskHandle_t* out) {
  BaseType_t ok = xTaskCreateWithCaps(fn, name, stackBytes, arg, prio, out,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (ok == pdPASS) return ok;
  return xTaskCreateWithCaps(fn, name, stackBytes, arg, prio, out,
                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// Pair with createTask. Prefer calling from another task; self-delete with
// nullptr is supported by IDF for WithCaps tasks.
inline void deleteTask(TaskHandle_t handle) { vTaskDeleteWithCaps(handle); }
#endif

}  // namespace M4Psram
