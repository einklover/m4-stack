#include "M4RuntimeMemory.h"

#if defined(CROSSPOINT_MURPHY_M4)
#include <Arduino.h>
#include <MemoryManager.h>

namespace {
M4MemoryPressure mapPressure(freeink::MemPressure pressure) {
  switch (pressure) {
    case freeink::MemPressure::Soft:
      return M4MemoryPressure::Soft;
    case freeink::MemPressure::Hard:
      return M4MemoryPressure::Hard;
    case freeink::MemPressure::None:
    default:
      return M4MemoryPressure::None;
  }
}

const char* pressureName(M4MemoryPressure pressure) {
  switch (pressure) {
    case M4MemoryPressure::Soft:
      return "soft";
    case M4MemoryPressure::Hard:
      return "hard";
    case M4MemoryPressure::None:
    default:
      return "none";
  }
}
}  // namespace

M4RuntimeMemorySnapshot m4CaptureRuntimeMemory() {
  auto& memory = freeink::MemoryManager::instance();
  M4RuntimeMemorySnapshot snapshot;
  snapshot.internalFree = memory.freeBytes(freeink::MemPool::Internal);
  snapshot.internalLargest = memory.largestFreeBlock(freeink::MemPool::Internal);
  snapshot.internalMinEver = memory.minEverFree(freeink::MemPool::Internal);
  snapshot.psramFree = memory.freeBytes(freeink::MemPool::Psram);
  snapshot.psramLargest = memory.largestFreeBlock(freeink::MemPool::Psram);
  snapshot.psramMinEver = memory.minEverFree(freeink::MemPool::Psram);
  snapshot.pressure = mapPressure(memory.pressure());
  return snapshot;
}

void m4LogRuntimeMemory(const char* stage) {
  const M4RuntimeMemorySnapshot snapshot = m4CaptureRuntimeMemory();
  // P0 intentionally leaves FreeInk watermarks unarmed. The pinned upstream
  // implementation computes an init-time reserved value but does not apply it
  // to pressure thresholds, so raw free/largest/min-ever metrics are the
  // authoritative baseline until that semantic is fixed or adapted.
  Serial.printf(
      "[%lu] [M4-MEM] stage=%s int_free=%u int_largest=%u int_min=%u "
      "psram_free=%u psram_largest=%u psram_min=%u pressure=%s pressure_armed=0 int_frag=%u%%\n",
      millis(), stage ? stage : "?", static_cast<unsigned>(snapshot.internalFree),
      static_cast<unsigned>(snapshot.internalLargest), static_cast<unsigned>(snapshot.internalMinEver),
      static_cast<unsigned>(snapshot.psramFree), static_cast<unsigned>(snapshot.psramLargest),
      static_cast<unsigned>(snapshot.psramMinEver), pressureName(snapshot.pressure),
      static_cast<unsigned>(m4InternalFragmentationPct(snapshot)));
}

#else

M4RuntimeMemorySnapshot m4CaptureRuntimeMemory() { return {}; }
void m4LogRuntimeMemory(const char*) {}

#endif
