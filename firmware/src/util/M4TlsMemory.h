#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>

namespace M4TlsMemory {

inline std::atomic<bool> gExternalActive{false};
inline bool externalActive() { return gExternalActive.load(std::memory_order_acquire); }

// The bundled SDK defaults to internal-only mbedTLS allocations. Two 16 KiB
// record buffers plus handshake state exhaust a fragmented internal heap even
// when the M4 has plenty of PSRAM. ESP-IDF also supports external TLS memory.
// Do not fall back to internal RAM: leave it for Wi-Fi, DMA and RTOS objects.
inline void* callocExternal(size_t count, size_t size) {
  if (size != 0 && count > SIZE_MAX / size) return nullptr;
  return heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// Shared admission policy for native HTTP and Lua plugin HTTPS APIs.
// The decision follows the allocator actually installed at startup.
inline bool resourcesAvailable() {
  constexpr unsigned internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  const size_t freeInternal = heap_caps_get_free_size(internal);
  if (externalActive()) {
    constexpr unsigned external = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    return freeInternal >= 32u * 1024u &&
           heap_caps_get_largest_free_block(internal) >= 8u * 1024u &&
           heap_caps_get_free_size(external) >= 128u * 1024u &&
           heap_caps_get_largest_free_block(external) >= 32u * 1024u;
  }
  // SDK-default mbedTLS needs an internal record buffer. Preserve its policy
  // on targets where the PSRAM allocator was not installed.
  if (freeInternal < 32u * 1024u) return false;
  if (freeInternal >= 96u * 1024u) return true;
  return heap_caps_get_largest_free_block(internal) >= 28u * 1024u;
}

// Call once from setup(), before networking/workers. Never swap allocator
// callbacks around individual requests: mbedTLS's callbacks are process-wide.
// heap_caps_free accepts both pre-existing internal and new external blocks.
inline bool install() {
  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == 0) return false;
  const bool ok = mbedtls_platform_set_calloc_free(callocExternal, heap_caps_free) == 0;
  gExternalActive.store(ok, std::memory_order_release);
  return ok;
}

}  // namespace M4TlsMemory
