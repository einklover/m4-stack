#pragma once

#include <stddef.h>
#include <stdint.h>

enum class M4MemoryPressure : uint8_t { None, Soft, Hard };

struct M4RuntimeMemorySnapshot {
  size_t internalFree = 0;
  size_t internalLargest = 0;
  size_t internalMinEver = 0;
  size_t psramFree = 0;
  size_t psramLargest = 0;
  size_t psramMinEver = 0;
  M4MemoryPressure pressure = M4MemoryPressure::None;
};

inline uint8_t m4InternalFragmentationPct(const M4RuntimeMemorySnapshot& snapshot) {
  if (snapshot.internalFree == 0) return 100;
  const size_t largest = snapshot.internalLargest > snapshot.internalFree
                             ? snapshot.internalFree
                             : snapshot.internalLargest;
  return static_cast<uint8_t>(100U - ((largest * 100U) / snapshot.internalFree));
}
