#include <assert.h>
#include <stdint.h>

#include "../../src/util/M4RuntimeMemorySnapshot.h"

int main() {
  const M4RuntimeMemorySnapshot healthy{
      100000, 80000, 70000, 7000000, 6000000, 5000000, M4MemoryPressure::None};
  assert(m4InternalFragmentationPct(healthy) == 20);

  const M4RuntimeMemorySnapshot fragmented{
      100000, 25000, 60000, 7000000, 6000000, 5000000, M4MemoryPressure::Soft};
  assert(m4InternalFragmentationPct(fragmented) == 75);

  const M4RuntimeMemorySnapshot clamped{
      100000, 120000, 60000, 7000000, 6000000, 5000000, M4MemoryPressure::None};
  assert(m4InternalFragmentationPct(clamped) == 0);

  const M4RuntimeMemorySnapshot empty{0, 0, 0, 0, 0, 0, M4MemoryPressure::Hard};
  assert(m4InternalFragmentationPct(empty) == 100);

  return 0;
}
