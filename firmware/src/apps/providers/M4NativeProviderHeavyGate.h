#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif

namespace M4NativeProviderHeavyGate {

// Small crash breadcrumb. It is deliberately a plain volatile word so the
// panic hook can copy it even after heap metadata has become unusable.
inline volatile uint32_t& diagnosticStage() {
  static volatile uint32_t stage = 0;
  return stage;
}

inline bool heapHealthy(uint32_t stage) {
  diagnosticStage() = stage;
#if defined(ARDUINO_ARCH_ESP32) && defined(M4_NATIVE_HEAP_DIAGNOSTIC)
  const bool ok = heap_caps_check_integrity_all(false);
  if (!ok) diagnosticStage() = stage | 0x80000000u;
  return ok;
#else
  (void)stage;
  return true;
#endif
}

// One process-wide gate for TLS/decode jobs. ESP32-S3 internal RAM is the
// scarce resource even when payload buffers live in PSRAM; two simultaneous
// handshakes can fragment/starve the internal heap and turn a safe streaming
// path into an OOM. Discovery/catalog/chapter workers all take this gate.
inline std::recursive_mutex& mutex() {
  static std::recursive_mutex g;
  return g;
}

// HTTP helpers also take this lock. A recursive mutex keeps existing outer
// chapter/catalog scopes valid while making direct HTTP callers safe too.
using Lock = std::unique_lock<std::recursive_mutex>;

inline bool tlsBlockAvailable() {
#if defined(ARDUINO_ARCH_ESP32)
  diagnosticStage() = 0x310;
  // Arduino-ESP32's bundled mbedTLS keeps the handshake buffers in internal
  // RAM. Prefer the largest contiguous free block over total free heap.
  //
  // After a first WeRead chapter, free total can still look healthy while the
  // largest contiguous block sits just under a hard 40KB guess — second
  // chapter then soft-fails with tls_internal_oom. Real handshakes on this
  // board succeed with ~28–32KB peaks once the session is warm; keep a floor
  // but do not over-reject fragmented heaps.
  //
  // NOTE: largest-free-block walks the TLSF free list — after a stack smash
  // that walk can panic. Prefer free_size precheck; skip the walk when free
  // is clearly ample.
  constexpr size_t kMinFreeInternal = 32u * 1024u;
  constexpr size_t kMinLargestBlock = 28u * 1024u;
  constexpr size_t kSkipWalkFree = 96u * 1024u;
  const size_t freeInternal =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (freeInternal < kMinFreeInternal) return false;
  if (freeInternal >= kSkipWalkFree) return true;
  diagnosticStage() = 0x311;
  return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >=
         kMinLargestBlock;
#else
  return true;
#endif
}

// Best-effort reclaim before a TLS-heavy chapter. Safe to call with or without
// an open transport session; the transport layer owns the actual sessionEnd.
inline void noteTlsPressure() {
  diagnosticStage() = 0x318;
}

}  // namespace M4NativeProviderHeavyGate
