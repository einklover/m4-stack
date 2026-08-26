#include <cassert>
#include <cstdio>

// Phase 4 regression: while the owner task is yield-busy (m4YieldToDebugBridge
// mid-frame poll), synthetic page-turn input must be rejected and MUST NOT be
// queued for later replay. After the busy window, accept resumes immediately.

#include "debug/M4SynthInputGate.h"

namespace {

void testRejectWhileYieldBusy() {
  assert(!M4SynthInputGate::acceptWhileOwnerIdle(true));
}

void testAcceptWhenOwnerIdle() {
  assert(M4SynthInputGate::acceptWhileOwnerIdle(false));
}

void testNoDeferredQueueSemantics() {
  // Policy is a pure gate: busy → reject, idle → accept. There is no FIFO,
  // no deferred:true ACK path, and no later replay. Callers map false to the
  // bridge busy error so hosts retry after the slow window.
  bool sawAccept = false;
  bool sawReject = false;
  for (int i = 0; i < 8; ++i) {
    const bool yieldBusy = (i % 2) == 0;
    if (M4SynthInputGate::acceptWhileOwnerIdle(yieldBusy)) {
      assert(!yieldBusy);
      sawAccept = true;
    } else {
      assert(yieldBusy);
      sawReject = true;
    }
  }
  assert(sawAccept && sawReject);
}

}  // namespace

int main() {
  testRejectWhileYieldBusy();
  testAcceptWhenOwnerIdle();
  testNoDeferredQueueSemantics();
  std::printf("synthetic input busy-reject policy: ALL PASS\n");
  return 0;
}
