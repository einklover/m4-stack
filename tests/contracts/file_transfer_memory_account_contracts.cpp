// P1E host contract for the file-transfer service-owned governance account.

#include <cassert>
#include <cstddef>

#include "M4FileTransferMemoryAccount.h"

int main() {
  M4FileTransferMemoryAccount account;

  assert(account.clean());
  assert(account.ownershipRecords() == 0);

  // Captive DNS is one idempotent service-owned reservation.
  assert(account.acquireDns());
  assert(account.acquireDns());
  assert(account.ownershipRecords() == 1);
  assert(!account.clean());

  // HTTP runtime is independently owned and can coexist with DNS.
  assert(account.acquireHttpRuntime());
  assert(account.acquireHttpRuntime());
  assert(account.ownershipRecords() == 2);

  account.releaseDns();
  assert(account.ownershipRecords() == 1);
  account.releaseHttpRuntime();
  assert(account.clean());

  // Partial-start rollback must restore the baseline deterministically.
  assert(account.acquireHttpRuntime());
  assert(!account.clean());
  account.releaseHttpRuntime();
  assert(account.clean());

  // Repeated service lifecycles must not accumulate ownership records.
  for (int i = 0; i < 32; ++i) {
    assert(account.acquireDns());
    assert(account.acquireHttpRuntime());
    account.releaseHttpRuntime();
    account.releaseDns();
    assert(account.clean());
    assert(account.ownershipRecords() == 0);
  }

  return 0;
}
