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

  // Single owner lifecycle coverage.
  assert(governance.acquireOwnership(16 * 1024));
  assert(governance.ownershipRecords() == 1);
  assert(governance.hasLifecycleLeak());

  governance.releaseOwnership(16 * 1024);
  assert(governance.ownershipRecords() == 0);
  assert(!governance.hasLifecycleLeak());
  assert(governance.remaining() == 64 * 1024);

  // Service/activity boundary simulation:
  // service owner acquires resources, then activity owner acquires resources.
  assert(governance.acquireOwnership(8 * 1024));
  assert(governance.acquireOwnership(4 * 1024));
  assert(governance.ownershipRecords() == 2);
  assert(governance.hasLifecycleLeak());

  // Activity exits first while service still owns resources.
  governance.releaseOwnership(4 * 1024);
  assert(governance.ownershipRecords() == 1);
  assert(governance.hasLifecycleLeak());

  // Service shutdown releases the final ownership record.
  governance.releaseOwnership(8 * 1024);
  assert(governance.ownershipRecords() == 0);
  assert(!governance.hasLifecycleLeak());

  // Fragmentation policy remains deterministic for host/QEMU checks.
  assert(governance.fragmentationStable());

  return 0;
}
