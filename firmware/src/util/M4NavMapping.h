#pragma once

#include <cstdint>

// Phase 1 (INV-N1): dependency-free logical-navigation mapping truth.
// Portrait policy, no orientation swap. Consumed by both production
// (MappedInputManager) and the Task 5 host behavioral test with bare g++.
enum class M4NavButton : uint8_t {
  Back = 0,
  Confirm,
  Left,
  Right,
  Up,
  Down,
  Power,
  PageBack,
  PageForward,
  NavNext,
  NavPrevious
};

// Down/Right are NavNext members, Up/Left are NavPrevious members.
// Nothing else is a member; the logical buttons themselves are never
// members so recursion over members always terminates.
[[nodiscard]] constexpr bool m4IsNavNextMember(M4NavButton b) {
  return b == M4NavButton::Down || b == M4NavButton::Right;
}

[[nodiscard]] constexpr bool m4IsNavPreviousMember(M4NavButton b) {
  return b == M4NavButton::Up || b == M4NavButton::Left;
}

// Member-expansion synth matcher: an injected member key satisfies its
// logical query; identity always satisfies; everything else is exact.
[[nodiscard]] constexpr bool m4SynthSatisfiesKey(M4NavButton synthKey, M4NavButton query) {
  if (synthKey == query) return true;
  if (query == M4NavButton::NavNext) return m4IsNavNextMember(synthKey);
  if (query == M4NavButton::NavPrevious) return m4IsNavPreviousMember(synthKey);
  return false;
}

// Edge folds over already-resolved, synth-inclusive member edges.
// Pressed and released share one fold shape: pure OR.
[[nodiscard]] constexpr bool m4NavNextEdge(bool downEdge, bool rightEdge) { return downEdge || rightEdge; }

[[nodiscard]] constexpr bool m4NavPreviousEdge(bool upEdge, bool leftEdge) { return upEdge || leftEdge; }

// Level folds over the same member sets: pure OR with no latch.
[[nodiscard]] constexpr bool m4NavNextLevel(bool downLevel, bool rightLevel) { return downLevel || rightLevel; }

[[nodiscard]] constexpr bool m4NavPreviousLevel(bool upLevel, bool leftLevel) { return upLevel || leftLevel; }
