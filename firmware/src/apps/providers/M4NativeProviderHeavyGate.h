#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#include "util/M4TlsMemory.h"
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
  return M4TlsMemory::resourcesAvailable();
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
