#include "apps/M4xInboxBatch.h"

#include <cassert>

int main() {
  assert(m4xInboxBatchDecide(false, false, 10, 0) == M4xInboxBatchAction::Keep);
  assert(m4xInboxBatchDecide(true, false, 10, 0) == M4xInboxBatchAction::Install);
  assert(m4xInboxBatchDecide(true, true, 10, 10) == M4xInboxBatchAction::SkipDelete);
  assert(m4xInboxBatchDecide(true, true, 9, 10) == M4xInboxBatchAction::SkipDelete);
  assert(m4xInboxBatchDecide(true, true, 11, 10) == M4xInboxBatchAction::Install);
  return 0;
}
