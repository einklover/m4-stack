#pragma once
// Bounded defer-queue for synthetic input (tap/swipe/key) received while the
// owner task is inside an activity frame (m4YieldToDebugBridge re-entry).
//
// Root cause this guards: synthetic events are one-frame (cleared at
// beginFrame()). An injection executed mid-frame — e.g. a tap drained from CDC
// RX by a yield-reentry poll() during a multi-second TTF paint — is erased by
// the NEXT beginFrame() before any activity input phase sees it, so the tap is
// silently dropped ("点击不知是否生效"). The bridge therefore queues the event
// (still replying ok immediately, preserving fast m4adb ACKs) and injects it
// from a later regular poll() window, right after beginFrame().
//
// Delivery contract (Bridge::deliverDeferredInput): FIFO order, at most ONE
// event injected per normal frame. MappedInputManager holds a single one-frame
// slot and enforces a minimum inject interval, so extra same-frame injections
// would be rejected as busy anyway. When the manager reports that transient
// busy/rate-limit the queue HEAD IS RETAINED and retried on a later frame
// instead of being silently lost; a successful inject pops exactly that one
// event. Overflow rejects at defer() time so the caller answers busy while the
// event never entered the queue.
//
// Pure policy header: no Arduino dependencies, unit-testable on host.

#include <cstddef>
#include <cstdint>

namespace M4SynthInputGate {

enum class Kind : uint8_t { Tap = 0, Swipe = 1, Key = 2 };

struct Input {
  Kind kind = Kind::Tap;
  // Tap: a=x b=y. Swipe: a=sx b=sy c=ex d=ey. Key: a=Button enum value.
  int16_t a = 0;
  int16_t b = 0;
  int16_t c = 0;
  int16_t d = 0;
};

template <size_t kCapacity>
class Gate {
 public:
  // Queue one input for upcoming regular windows. False ⇒ full, caller must
  // reject with busy so the host retries; nothing was enqueued or lost.
  bool defer(const Input& in) {
    if (count_ >= kCapacity) return false;
    q_[count_++] = in;
    return true;
  }

  // Attempt to deliver exactly the OLDEST event. sink(input) must perform the
  // real injection and return whether the manager accepted it. The head pops
  // only on success; on transient failure (busy / rate limit) it stays at the
  // front so FIFO order and exactly-once eventual delivery are preserved.
  // Returns whether one event was delivered this call.
  template <typename Sink>
  bool deliverOne(Sink&& sink) {
    if (count_ == 0) return false;
    if (!sink(q_[0])) return false;
    for (size_t i = 1; i < count_; ++i) q_[i - 1] = q_[i];
    q_[count_ - 1] = Input{};
    --count_;
    return true;
  }

  size_t size() const { return count_; }
  bool empty() const { return count_ == 0; }

  // Drop everything undelivered (session reset / auth disable). Events queued
  // but never delivered were ACKed; dropping here mirrors the previous
  // behaviour of losing them rather than injecting into a fresh session.
  void clear() {
    for (size_t i = 0; i < count_; ++i) q_[i] = Input{};
    count_ = 0;
  }

 private:
  Input q_[kCapacity] = {};
  size_t count_ = 0;
};

}  // namespace M4SynthInputGate
