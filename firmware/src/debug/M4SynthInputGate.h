#pragma once
// Phase 4 synthetic-input busy policy (host-testable, no Arduino deps).
//
// While the owner task is inside an activity frame (m4YieldToDebugBridge
// re-entry → yieldContext), synthetic tap/swipe/key/back must be rejected
// with busy and MUST NOT be queued for later replay. A deferred FIFO was
// tried earlier; delayed surprise multi-page turns after the slow
// first-page index window were worse than a transient busy ACK.
//
// Contract:
//   * yieldBusy == true  → reject (host retries after the window)
//   * yieldBusy == false → accept; Bridge injects on the regular poll path
//     (post-beginFrame), so the activity input phase can see the event
// Once loading/indexing finishes, the next regular poll accepts page turns
// immediately — no catch-up burst from a queue.

namespace M4SynthInputGate {

inline bool acceptWhileOwnerIdle(bool yieldBusy) { return !yieldBusy; }

}  // namespace M4SynthInputGate
