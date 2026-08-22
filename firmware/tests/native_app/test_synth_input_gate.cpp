#include <cassert>
#include <cstdint>
#include <cstdio>

// Phase 4 regression: synthetic input received mid-frame (m4YieldToDebugBridge
// yield-context poll) must be deferred, delivered exactly once in a later
// regular window at most ONE per frame, retained across transient busy/rate
// limit (never silently lost), and rejected up front when invalid. Exercises
// the pure policy in debug/M4SynthInputGate.h that the M4SerialDebugBridge
// tap/swipe/key handlers rely on.

#include "debug/M4SynthInputGate.h"

using M4SynthInputGate::Gate;
using M4SynthInputGate::Input;
using M4SynthInputGate::Kind;

namespace {

// Mirrors Bridge::deliverDeferredInput + MappedInputManager inject contract:
// single one-frame slot; success pops the event, busy/rate-limit keeps it.
template <size_t N>
struct FakeBridge {
  Gate<N> gate;
  bool slotBusy = false;      // synthetic slot occupied this frame
  bool rateLimited = false;   // minimum-inject-interval rejection
  Kind applied[N] = {};
  size_t appliedCount = 0;

  bool defer(const Input& in) { return gate.defer(in); }

  bool inject(const Input& in) {
    if (slotBusy || rateLimited || appliedCount >= N) return false;
    slotBusy = true;
    applied[appliedCount++] = in.kind;
    return true;
  }

  // One regular poll window: beginFrame clears the slot, then deliver one.
  void frame() {
    slotBusy = false;
    (void)gate.deliverOne([this](const Input& in) { return inject(in); });
  }
};

void testExactlyOnceDelivery() {
  Gate<4> g;
  assert(g.defer({Kind::Tap, 400, 420}));
  int deliveries = 0;
  Input seen{};
  assert(g.deliverOne([&](const Input& in) {
    ++deliveries;
    seen = in;
    return true;
  }));
  assert(deliveries == 1);
  assert(seen.kind == Kind::Tap && seen.a == 400 && seen.b == 420);
  assert(g.empty());
  // A second delivery attempt hands over nothing (exactly-once).
  deliveries = 0;
  assert(!g.deliverOne([&](const Input&) {
    ++deliveries;
    return true;
  }));
  assert(deliveries == 0);
}

void testFifoAndPayloadPreservation() {
  Gate<8> g;
  assert(g.defer({Kind::Tap, 1, 2}));
  assert(g.defer({Kind::Swipe, 3, 4, 5, 6}));
  assert(g.defer({Kind::Key, 7}));
  Input got[3] = {};
  int n = 0;
  while (!g.empty()) {
    assert(g.deliverOne([&](const Input& in) {
      got[n] = in;
      ++n;
      return true;
    }));
  }
  assert(n == 3);
  assert(got[0].kind == Kind::Tap && got[0].a == 1 && got[0].b == 2);
  assert(got[1].kind == Kind::Swipe && got[1].a == 3 && got[1].b == 4 && got[1].c == 5 &&
         got[1].d == 6);
  assert(got[2].kind == Kind::Key && got[2].a == 7);
}

void testAtMostOnePerFrame() {
  FakeBridge<8> b;
  assert(b.defer({Kind::Tap, 1, 1}));
  assert(b.defer({Kind::Tap, 2, 2}));
  b.frame();
  assert(b.appliedCount == 1);  // second event waits for the next frame
  assert(b.gate.size() == 1);
  b.frame();
  assert(b.appliedCount == 2);
  assert(b.gate.empty());
}

void testHeadRetainedOnTransientBusy() {
  FakeBridge<8> b;
  assert(b.defer({Kind::Tap, 10, 10}));
  assert(b.defer({Kind::Key, 3}));
  // Frame where the manager is rate-limited: nothing delivered, nothing
  // dropped — both events survive.
  b.rateLimited = true;
  b.frame();
  b.rateLimited = false;
  assert(b.appliedCount == 0);
  assert(b.gate.size() == 2);
  // Recovery delivers the OLDEST first (FIFO preserved through the failure).
  b.frame();
  assert(b.appliedCount == 1 && b.applied[0] == Kind::Tap);
  b.frame();
  assert(b.appliedCount == 2 && b.applied[1] == Kind::Key);
  assert(b.gate.empty());
}

void testOverflowRejectsExplicitly() {
  Gate<2> g;
  assert(g.defer({Kind::Tap, 1, 1}));
  assert(g.defer({Kind::Tap, 2, 2}));
  assert(!g.defer({Kind::Tap, 3, 3}));  // caller replies busy; host retries
  assert(g.size() == 2);
  g.clear();
  assert(g.empty());
  assert(g.defer({Kind::Tap, 4, 4}));  // room again after reset
}

void testClearDropsUndelivered() {
  Gate<4> g;
  assert(g.defer({Kind::Key, 1}));
  assert(g.defer({Kind::Key, 2}));
  g.clear();
  int deliveries = 0;
  assert(!g.deliverOne([&](const Input&) {
    ++deliveries;
    return true;
  }));
  assert(deliveries == 0);
  assert(g.empty());
}

// ---- Validation policy mirrored from handleReq (reject before defer/ACK) ----

struct Rect {
  int w;
  int h;
};

bool validTap(const Rect& r, int x, int y) {
  return x >= 0 && y >= 0 && x < r.w && y < r.h;
}

bool validSwipe(const Rect& r, int sx, int sy, int ex, int ey) {
  return sx >= 0 && sy >= 0 && ex >= 0 && ey >= 0 && sx < r.w && ex < r.w && sy < r.h && ey < r.h &&
         !(sx == ex && sy == ey);
}

void testInvalidTapRejectedBeforeDefer() {
  const Rect screen{480, 800};
  Gate<4> g;
  assert(!validTap(screen, -1, 5));
  assert(!validTap(screen, 5, -1));
  assert(!validTap(screen, 480, 100));  // x == width → out of bounds
  assert(!validTap(screen, 100, 800));
  assert(validTap(screen, 0, 0));
  assert(validTap(screen, 479, 799));
  // The bridge defers only validated inputs; gate stays empty for invalid ones.
  const int badTaps[2][2] = {{-1, 5}, {480, 100}};
  for (const auto& xy : badTaps) {
    if (validTap(screen, xy[0], xy[1])) {
      assert(g.defer({Kind::Tap, static_cast<int16_t>(xy[0]), static_cast<int16_t>(xy[1])}));
    }
  }
  assert(g.empty());
}

void testValidSwipeAcceptedDegenerateRejected() {
  struct SwipeCoords {
    int sx, sy, ex, ey;
  };
  const Rect screen{480, 800};
  Gate<4> g;
  assert(!validSwipe(screen, 10, 10, 10, 10));   // zero-length trajectory
  assert(!validSwipe(screen, -1, 0, 50, 50));
  assert(!validSwipe(screen, 0, 0, 481, 50));
  assert(validSwipe(screen, 400, 420, 80, 420));
  if (validSwipe(screen, 400, 420, 80, 420)) {
    assert(g.defer({Kind::Swipe, 400, 420, 80, 420}));
  }
  SwipeCoords seen{0, 0, 0, 0};
  assert(g.deliverOne([&](const Input& in) {
    seen = {in.a, in.b, in.c, in.d};
    return true;
  }));
  assert(seen.sx == 400 && seen.sy == 420 && seen.ex == 80 && seen.ey == 420);
}

}  // namespace

int main() {
  testExactlyOnceDelivery();
  testFifoAndPayloadPreservation();
  testAtMostOnePerFrame();
  testHeadRetainedOnTransientBusy();
  testOverflowRejectsExplicitly();
  testClearDropsUndelivered();
  testInvalidTapRejectedBeforeDefer();
  testValidSwipeAcceptedDegenerateRejected();
  std::printf("synthetic input gate regression: ALL PASS\n");
  return 0;
}
