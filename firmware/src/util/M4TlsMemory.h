#pragma once

#include <cstddef>
#include <cstdint>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>

namespace M4TlsMemory {

// The bundled SDK defaults to internal-only mbedTLS allocations. Two 16 KiB
// record buffers plus handshake state exhaust a fragmented internal heap even
// when the M4 has plenty of PSRAM. ESP-IDF also supports external TLS memory.
// Do not fall back to internal RAM: leave it for Wi-Fi, DMA and RTOS objects.
inline void* callocExternal(size_t count, size_t size) {
  if (size != 0 && count > SIZE_MAX / size) return nullptr;
  return heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// Call once from setup(), before networking/workers. Never swap allocator
// callbacks around individual requests: mbedTLS's callbacks are process-wide.
// heap_caps_free accepts both pre-existing internal and new external blocks.
inline bool install() {
  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == 0) return false;
  return mbedtls_platform_set_calloc_free(callocExternal, heap_caps_free) == 0;
}

}  // namespace M4TlsMemory
