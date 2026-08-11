#pragma once

// Single-owner event pump + lifecycle policy for M4x Lua runtime.
//
// Production AppRuntimeActivity uses OwnerLifecycle decisions and the same
// Stop/drop/done ordering as OwnerLoop (host tests). FreeRTOS provides the
// queue; EventRing is single-thread only for host tests.
//
// Lifecycle contract:
//  1) UI: requestStop → cancel (atomic) + enqueue Stop
//  2) Owner: drop non-Stop, run Stop handler (host.stop only on owner)
//  3) Owner epilogue: no further member access, then publishDone() release-store
//  4) UI: observe done (acquire), then free queue/mutex / destroy activity
//
// After publishDone(), the owner task must not touch the activity object.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace M4xRuntime {

enum class EventType : uint8_t {
  Start = 0,
  Stop,
  Draw,
  Key,
  Touch,
  Tick,
};

struct Event {
  EventType type = EventType::Tick;
  int x = 0;
  int y = 0;
  char key[24] = {};
  char phase[12] = {};

  static Event makeKey(const char* name) {
    Event e;
    e.type = EventType::Key;
    if (name) std::strncpy(e.key, name, sizeof(e.key) - 1);
    return e;
  }
  static Event makeTouch(int x, int y, const char* phase) {
    Event e;
    e.type = EventType::Touch;
    e.x = x;
    e.y = y;
    if (phase)
      std::strncpy(e.phase, phase, sizeof(e.phase) - 1);
    else
      std::strncpy(e.phase, "tap", sizeof(e.phase) - 1);
    return e;
  }
  static Event makeDraw() {
    Event e;
    e.type = EventType::Draw;
    return e;
  }
  static Event makeStart() {
    Event e;
    e.type = EventType::Start;
    return e;
  }
  static Event makeStop() {
    Event e;
    e.type = EventType::Stop;
    return e;
  }
  static Event makeTick() {
    Event e;
    e.type = EventType::Tick;
    return e;
  }
};

// ---------------------------------------------------------------------------
// Shared production + test policy (pure decisions / atomics).
// ---------------------------------------------------------------------------
struct OwnerLifecycle {
  std::atomic<bool> stopRequested{false};
  // True only after owner finished ALL accesses to the session object.
  std::atomic<bool> ownerDone{false};

  // Owner-thread-only bookkeeping (not shared).
  bool hostStopped = false;

  void requestStop() { stopRequested.store(true, std::memory_order_relaxed); }

  bool isStopRequested() const { return stopRequested.load(std::memory_order_relaxed); }

  // After stop requested, drop backlog so teardown is not delayed.
  static bool shouldDropEvent(bool stopRequested, EventType t) {
    return stopRequested && t != EventType::Stop;
  }

  bool shouldDrop(EventType t) const { return shouldDropEvent(isStopRequested(), t); }

  // Inject Stop when stop was requested but the Stop event is not available.
  static bool shouldInjectStop(bool stopRequested, bool queueEmpty) {
    return stopRequested && queueEmpty;
  }

  // Call ONLY at true owner epilogue: host already stopped, no further
  // reads/writes of session members. After this release-store, owner must not
  // touch the activity / handler object.
  void publishDone() { ownerDone.store(true, std::memory_order_release); }

  bool isDone() const { return ownerDone.load(std::memory_order_acquire); }

  void reset() {
    stopRequested.store(false, std::memory_order_relaxed);
    ownerDone.store(false, std::memory_order_relaxed);
    hostStopped = false;
  }
};

// Fixed ring — single-thread only (host tests).
class EventRing {
 public:
  static constexpr size_t kCapacity = 32;

  bool empty() const { return count_ == 0; }
  bool full() const { return count_ == kCapacity; }
  size_t size() const { return count_; }

  bool push(const Event& e) {
    if (full()) {
      head_ = (head_ + 1) % kCapacity;
      --count_;
      dropped_ = true;
    }
    buf_[tail_] = e;
    tail_ = (tail_ + 1) % kCapacity;
    ++count_;
    return true;
  }

  bool pop(Event& out) {
    if (empty()) return false;
    out = buf_[head_];
    head_ = (head_ + 1) % kCapacity;
    --count_;
    return true;
  }

  void clear() {
    head_ = tail_ = count_ = 0;
    dropped_ = false;
  }

  bool hadDrop() const { return dropped_; }

 private:
  Event buf_[kCapacity]{};
  size_t head_ = 0;
  size_t tail_ = 0;
  size_t count_ = 0;
  bool dropped_ = false;
};

// Handler:
//   void onRuntimeEvent(const Event&);  // Stop: stop host only; do NOT publish done
//   void onCancelRequested();
//
// OwnerLoop uses OwnerLifecycle: publishDone only after Stop handler returns
// and before step returns Stopped — tests assert no further handler use.

template <typename Handler>
class OwnerLoop {
 public:
  explicit OwnerLoop(Handler& handler) : handler_(handler) {}

  OwnerLifecycle& life() { return life_; }
  const OwnerLifecycle& life() const { return life_; }

  bool post(const Event& e) { return ring_.push(e); }

  void requestStop() {
    life_.requestStop();
    handler_.onCancelRequested();
    ring_.push(Event::makeStop());
  }

  bool stopRequested() const { return life_.isStopRequested(); }
  bool stopAck() const { return life_.isDone(); }  // alias: done == safe for UI free
  bool isDone() const { return life_.isDone(); }
  bool inCallback() const { return inCallback_; }

  enum class StepResult : uint8_t { Idle, Processed, Stopped };

  StepResult step() {
    if (life_.isDone()) return StepResult::Stopped;

    Event e;
    if (!ring_.pop(e)) {
      if (OwnerLifecycle::shouldInjectStop(life_.isStopRequested(), ring_.empty())) {
        return runStopOnOwner(/*synthetic=*/true);
      }
      return StepResult::Idle;
    }

    if (life_.shouldDrop(e.type)) {
      return StepResult::Processed;
    }

    if (e.type == EventType::Stop) {
      return runStopOnOwner(/*synthetic=*/false);
    }

    inCallback_ = true;
    handler_.onRuntimeEvent(e);
    inCallback_ = false;
    return StepResult::Processed;
  }

  size_t runUntilStopped(size_t maxSteps = 0) {
    size_t n = 0;
    while (!life_.isDone()) {
      if (maxSteps && n >= maxSteps) break;
      const StepResult r = step();
      if (r == StepResult::Idle && !life_.isStopRequested()) break;
      if (r == StepResult::Stopped) break;
      ++n;
    }
    return n;
  }

  EventRing& ring() { return ring_; }

 private:
  StepResult runStopOnOwner(bool /*synthetic*/) {
    // Owner runs Stop handler (host teardown). Handler must NOT publish done.
    inCallback_ = true;
    handler_.onRuntimeEvent(Event::makeStop());
    inCallback_ = false;
    life_.hostStopped = true;
    // True epilogue: no further handler_/session access after this store.
    life_.publishDone();
    return StepResult::Stopped;
  }

  Handler& handler_;
  EventRing ring_;
  OwnerLifecycle life_;
  bool inCallback_ = false;
};

template <typename Handler>
class Pump {
 public:
  explicit Pump(Handler& handler) : handler_(handler) {}
  bool post(const Event& e) { return ring_.push(e); }
  bool processNext() {
    Event e;
    if (!ring_.pop(e)) return false;
    handler_.onRuntimeEvent(e);
    return true;
  }
  size_t processAll(size_t maxEvents = 0) {
    size_t n = 0;
    while ((maxEvents == 0 || n < maxEvents) && processNext()) ++n;
    return n;
  }

 private:
  Handler& handler_;
  EventRing ring_;
};

}  // namespace M4xRuntime
