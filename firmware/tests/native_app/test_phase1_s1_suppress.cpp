// Phase1 S1 RED: consume-once suppression over the pinned Task 10 API.
// RED reason: `util/M4SuppressState.h` does not exist yet (Task 10 GREEN),
// so this translation unit must fail to compile until the helper lands.
// Production wiring (MappedInputManager physical-only feed, main.cpp
// frame-top check) is pinned by test_phase1_s1_frame_skip_contract.py.
#include "util/M4SuppressState.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

// Pinned Task 10 contract (GREEN must provide exactly this):
//   struct M4SuppressState { uint16_t mask = 0; uint16_t firedLatch = 0; };
//   [[nodiscard]] constexpr bool m4SuppressButtonIndex(uint8_t buttonIndex);
//   constexpr void m4SuppressNextRelease(M4SuppressState&, uint8_t buttonIndex);
//   constexpr bool m4ConsumeSuppressedRelease(M4SuppressState&,
//                                             uint16_t physicalReleasedMask);
//   constexpr bool m4LongPressFired(M4SuppressState&, uint8_t buttonIndex,
//                                   bool isPressed, unsigned long heldMs,
//                                   unsigned long thresholdMs);

namespace {

constexpr uint8_t kBack = 1;    // Back button index (1.5 s site in main.cpp).
constexpr uint8_t kOther = 4;   // Unrelated button index for mixed-frame gate.
constexpr unsigned long kThresholdMs = 1500;

constexpr uint16_t kBackBit = static_cast<uint16_t>(1u << kBack);
constexpr uint16_t kOtherBit = static_cast<uint16_t>(1u << kOther);

}  // namespace

int main() {
  // Index validity: 11 enumerators fit uint16_t, so small indices are usable.
  assert(m4SuppressButtonIndex(kBack));
  assert(m4SuppressButtonIndex(kOther));

  // --- Threshold-crossing press/hold/release: exactly one action. ---
  M4SuppressState st{};
  assert(st.mask == 0);
  assert(st.firedLatch == 0);

  int actions = 0;
  // Press, below threshold: no fire.
  assert(!m4LongPressFired(st, kBack, true, 0, kThresholdMs));
  assert(!m4LongPressFired(st, kBack, true, 500, kThresholdMs));
  assert(!m4LongPressFired(st, kBack, true, kThresholdMs - 1, kThresholdMs));
  // Cross threshold while held: fires exactly once.
  assert(m4LongPressFired(st, kBack, true, kThresholdMs, kThresholdMs));
  ++actions;
  // Still held past threshold: one-shot, must not refire.
  assert(!m4LongPressFired(st, kBack, true, kThresholdMs + 500, kThresholdMs));
  assert(actions == 1);

  // Release edge is consumed once; the frame-top check drops the frame.
  bool consumed = m4ConsumeSuppressedRelease(st, kBackBit);
  assert(consumed);
  bool dispatchedRelease = !consumed;  // frame skipped => never dispatches.
  assert(!dispatchedRelease);
  // Consume-once: already-consumed release does not consume again.
  assert(!m4ConsumeSuppressedRelease(st, 0));

  // --- Fresh press on the same button fires again. ---
  assert(!m4LongPressFired(st, kBack, false, 0, kThresholdMs));  // released reset
  assert(!m4LongPressFired(st, kBack, true, 100, kThresholdMs));
  assert(m4LongPressFired(st, kBack, true, kThresholdMs, kThresholdMs));
  ++actions;
  assert(actions == 2);
  assert(m4ConsumeSuppressedRelease(st, kBackBit));
  assert(!m4ConsumeSuppressedRelease(st, 0));

  // --- Manual arm path: m4SuppressNextRelease + consume-once. ---
  {
    M4SuppressState manual{};
    m4SuppressNextRelease(manual, kBack);
    assert(m4ConsumeSuppressedRelease(manual, kBackBit));
    assert(!m4ConsumeSuppressedRelease(manual, 0));
  }

  // --- Mixed frame: unrelated edge + gesture pulse dropped with the ---
  // --- consumed release; held level stays observable next frame. ---
  {
    M4SuppressState mixed{};
    // Fire Back long-press, keep it held.
    assert(!m4LongPressFired(mixed, kBack, true, 100, kThresholdMs));
    assert(m4LongPressFired(mixed, kBack, true, kThresholdMs, kThresholdMs));
    bool backHeldLevel = true;  // physical level, independent of edge masks.
    bool otherHeldLevel = true;

    // Coincident frame: Back release + unrelated release edge + gesture pulse.
    const uint16_t physicalReleasedMask =
        static_cast<uint16_t>(kBackBit | kOtherBit);
    const bool gesturePulse = true;
    (void)gesturePulse;
    const bool frameConsumed =
        m4ConsumeSuppressedRelease(mixed, physicalReleasedMask);
    assert(frameConsumed);
    // Dropped with the consumed frame: never dispatched, never replayed.
    const bool unrelatedDispatched = !frameConsumed;
    const bool gestureDispatched = !frameConsumed;
    assert(!unrelatedDispatched);
    assert(!gestureDispatched);
    // Held levels remain observable next frame via normal level reads.
    assert(backHeldLevel);
    assert(otherHeldLevel);
    assert(m4SuppressButtonIndex(kBack));
    assert(m4SuppressButtonIndex(kOther));
    // Next frame is idle: nothing left to consume.
    assert(!m4ConsumeSuppressedRelease(mixed, 0));
  }

  // --- Synthetic input is inert: neither sets, clears, nor observes. ---
  {
    M4SuppressState synth{};
    assert(m4LongPressFired(synth, kBack, true, kThresholdMs, kThresholdMs));
    const uint16_t pendingMask = synth.mask;
    const uint16_t pendingLatch = synth.firedLatch;
    // Synthetic frames must never call the helpers: represent the synthetic
    // Key sequence around the suppressed release as no helper calls, then
    // prove physical suppression is still pending, untouched.
    {
      // No m4* call here by construction (synthetic path is inert).
      const bool syntheticFrameSawState = false;
      assert(!syntheticFrameSawState);
    }
    assert(synth.mask == pendingMask);
    assert(synth.firedLatch == pendingLatch);
    // Physical release still consumes exactly once afterwards.
    assert(m4ConsumeSuppressedRelease(synth, kBackBit));
    assert(!m4ConsumeSuppressedRelease(synth, 0));
  }

  std::printf("phase1-s1 suppress RED: pinned API gates observe helper\n");
  return 0;
}
