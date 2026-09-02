// P2 memory governance host/QEMU contract.
// GREEN migration: validate deterministic budget and lifecycle ownership invariants.

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

  // Ownership lifecycle acquire creates a tracked owner.
  assert(governance.acquireOwnership(16 * 1024));
  assert(governance.ownershipRecords() == 1);
  assert(governance.hasLifecycleLeak());

  // Ownership release clears lifecycle leak state and restores capacity.
  governance.releaseOwnership(16 * 1024);
  assert(governance.ownershipRecords() == 0);
  assert(!governance.hasLifecycleLeak());
  assert(governance.remaining() == 64 * 1024);

  // Fragmentation policy remains deterministic for host/QEMU checks.
  assert(governance.fragmentationStable());

  return 0;
}
