// P2 memory governance host/QEMU contract.
// GREEN migration: validate the deterministic governance implementation.

#include <cassert>
#include <cstddef>

#include "M4MemoryGovernance.h"

int main() {
  M4MemoryGovernance governance(64 * 1024);

  // Reserve within budget succeeds.
  assert(governance.reserve(32 * 1024));
  assert(governance.remaining() == 32 * 1024);

  // Over-budget reservation is rejected deterministically.
  assert(!governance.reserve(40 * 1024));

  // Release restores bounded capacity.
  governance.release(32 * 1024);
  assert(governance.remaining() == 64 * 1024);

  // Fragmentation policy remains deterministic for host/QEMU checks.
  assert(governance.fragmentationStable());

  return 0;
}
