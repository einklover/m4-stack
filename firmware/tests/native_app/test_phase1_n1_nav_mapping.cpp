// Task 5 (INV-N1) RED — behavioral navigation-mapping test.
//
// Covers the planned dependency-free helper `util/M4NavMapping.h` that Task 6
// production code consumes (same bare-g++ pure-header pattern as
// test_progressive_http_state). RED gate: this file must FAIL TO COMPILE
// before Task 6 because the header does not exist yet; after Task 6 it must
// pass and prove Down/Right -> NavNext and Up/Left -> NavPrevious equivalence
// through synthetic-inclusive edge/level folds.
//
// Build: g++ -std=c++20 -Ifirmware/src \
//          firmware/tests/native_app/test_phase1_n1_nav_mapping.cpp \
//          -o /tmp/phase1_n1_nav && /tmp/phase1_n1_nav
#include "util/M4NavMapping.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <type_traits>

namespace {

constexpr M4NavButton kAllButtons[] = {
    M4NavButton::Back,       M4NavButton::Confirm, M4NavButton::Left,  M4NavButton::Right,
    M4NavButton::Up,         M4NavButton::Down,    M4NavButton::Power, M4NavButton::PageBack,
    M4NavButton::PageForward, M4NavButton::NavNext, M4NavButton::NavPrevious,
};
constexpr int kButtonCount = 11;

// --- Enum: NavNext/NavPrevious appended after PageForward, ordinals 0-8 pinned.
void enum_ordinals() {
  static_assert(static_cast<int>(M4NavButton::Back) == 0);
  static_assert(static_cast<int>(M4NavButton::Confirm) == 1);
  static_assert(static_cast<int>(M4NavButton::Left) == 2);
  static_assert(static_cast<int>(M4NavButton::Right) == 3);
  static_assert(static_cast<int>(M4NavButton::Up) == 4);
  static_assert(static_cast<int>(M4NavButton::Down) == 5);
  static_assert(static_cast<int>(M4NavButton::Power) == 6);
  static_assert(static_cast<int>(M4NavButton::PageBack) == 7);
  static_assert(static_cast<int>(M4NavButton::PageForward) == 8);
  static_assert(static_cast<int>(M4NavButton::NavNext) == 9);
  static_assert(static_cast<int>(M4NavButton::NavPrevious) == 10);
  static_assert(std::is_same<std::underlying_type<M4NavButton>::type, uint8_t>::value);
  assert(kButtonCount == 11);
}

// --- Member expansion: Down/Right are NavNext members, Up/Left NavPrevious.
void member_expansion() {
  assert(m4IsNavNextMember(M4NavButton::Down));
  assert(m4IsNavNextMember(M4NavButton::Right));
  assert(m4IsNavPreviousMember(M4NavButton::Up));
  assert(m4IsNavPreviousMember(M4NavButton::Left));
  // Nothing else is a member — cross-axis, non-directionals, and the logical
  // buttons themselves (recursion terminates because members are never logical).
  for (const M4NavButton b : kAllButtons) {
    const bool expectNext = (b == M4NavButton::Down || b == M4NavButton::Right);
    const bool expectPrev = (b == M4NavButton::Up || b == M4NavButton::Left);
    assert(m4IsNavNextMember(b) == expectNext);
    assert(m4IsNavPreviousMember(b) == expectPrev);
  }
  assert(!m4IsNavNextMember(M4NavButton::NavNext));
  assert(!m4IsNavNextMember(M4NavButton::NavPrevious));
  assert(!m4IsNavPreviousMember(M4NavButton::NavNext));
  assert(!m4IsNavPreviousMember(M4NavButton::NavPrevious));
}

// --- Edge folds (pressed AND released share one fold shape): the logical edge
// equals the OR of the already-resolved, synth-inclusive member edges.
void edge_folds() {
  for (int d = 0; d <= 1; ++d) {
    for (int r = 0; r <= 1; ++r) {
      const bool downEdge = (d != 0);
      const bool rightEdge = (r != 0);
      assert(m4NavNextEdge(downEdge, rightEdge) == (downEdge || rightEdge));
    }
  }
  for (int u = 0; u <= 1; ++u) {
    for (int l = 0; l <= 1; ++l) {
      const bool upEdge = (u != 0);
      const bool leftEdge = (l != 0);
      assert(m4NavPreviousEdge(upEdge, leftEdge) == (upEdge || leftEdge));
    }
  }
}

// --- Level folds (isPressed level-OR over the same member sets).
void level_folds() {
  for (int d = 0; d <= 1; ++d) {
    for (int r = 0; r <= 1; ++r) {
      const bool downLevel = (d != 0);
      const bool rightLevel = (r != 0);
      assert(m4NavNextLevel(downLevel, rightLevel) == (downLevel || rightLevel));
    }
  }
  for (int u = 0; u <= 1; ++u) {
    for (int l = 0; l <= 1; ++l) {
      const bool upLevel = (u != 0);
      const bool leftLevel = (l != 0);
      assert(m4NavPreviousLevel(upLevel, leftLevel) == (upLevel || leftLevel));
    }
  }
  // Held-level persistence is pure OR with no latch: releasing one member
  // while the other stays held keeps the logical level true.
  assert(m4NavNextLevel(false, true));
  assert(m4NavPreviousLevel(true, false));
  assert(!m4NavNextLevel(false, false));
  assert(!m4NavPreviousLevel(false, false));
}

// --- Member-expansion synth matcher plus direct logical injection.
void synth_matcher() {
  // Identity: every button satisfies its own query.
  for (const M4NavButton b : kAllButtons) {
    assert(m4SynthSatisfiesKey(b, b));
  }
  // Injected member keys satisfy their logical query.
  assert(m4SynthSatisfiesKey(M4NavButton::Down, M4NavButton::NavNext));
  assert(m4SynthSatisfiesKey(M4NavButton::Right, M4NavButton::NavNext));
  assert(m4SynthSatisfiesKey(M4NavButton::Up, M4NavButton::NavPrevious));
  assert(m4SynthSatisfiesKey(M4NavButton::Left, M4NavButton::NavPrevious));
  // Cross-axis and non-member injections never satisfy a logical query.
  assert(!m4SynthSatisfiesKey(M4NavButton::Down, M4NavButton::NavPrevious));
  assert(!m4SynthSatisfiesKey(M4NavButton::Right, M4NavButton::NavPrevious));
  assert(!m4SynthSatisfiesKey(M4NavButton::Up, M4NavButton::NavNext));
  assert(!m4SynthSatisfiesKey(M4NavButton::Left, M4NavButton::NavNext));
  assert(!m4SynthSatisfiesKey(M4NavButton::Back, M4NavButton::NavNext));
  assert(!m4SynthSatisfiesKey(M4NavButton::Confirm, M4NavButton::NavPrevious));
  assert(!m4SynthSatisfiesKey(M4NavButton::Power, M4NavButton::NavNext));
  assert(!m4SynthSatisfiesKey(M4NavButton::PageBack, M4NavButton::NavPrevious));
  assert(!m4SynthSatisfiesKey(M4NavButton::PageForward, M4NavButton::NavNext));
  // Logical injections satisfy only their own logical query, never a member
  // query and never the opposite logical (directional, not symmetric).
  assert(m4SynthSatisfiesKey(M4NavButton::NavNext, M4NavButton::NavNext));
  assert(m4SynthSatisfiesKey(M4NavButton::NavPrevious, M4NavButton::NavPrevious));
  assert(!m4SynthSatisfiesKey(M4NavButton::NavNext, M4NavButton::Down));
  assert(!m4SynthSatisfiesKey(M4NavButton::NavNext, M4NavButton::Right));
  assert(!m4SynthSatisfiesKey(M4NavButton::NavPrevious, M4NavButton::Up));
  assert(!m4SynthSatisfiesKey(M4NavButton::NavPrevious, M4NavButton::Left));
  assert(!m4SynthSatisfiesKey(M4NavButton::NavNext, M4NavButton::NavPrevious));
  assert(!m4SynthSatisfiesKey(M4NavButton::NavPrevious, M4NavButton::NavNext));
  // Member-to-member injections stay exact (Down is not Right).
  assert(!m4SynthSatisfiesKey(M4NavButton::Down, M4NavButton::Right));
  assert(!m4SynthSatisfiesKey(M4NavButton::Up, M4NavButton::Left));
}

// --- Full 11 x 11 injection matrix against the independent member-set oracle.
void full_injection_matrix() {
  for (const M4NavButton query : kAllButtons) {
    for (const M4NavButton synth : kAllButtons) {
      const bool expected = (synth == query) ||
                            (query == M4NavButton::NavNext && m4IsNavNextMember(synth)) ||
                            (query == M4NavButton::NavPrevious && m4IsNavPreviousMember(synth));
      assert(m4SynthSatisfiesKey(synth, query) == expected);
    }
  }
}

// --- 11 enumerators fit a uint16_t edge/suppression mask with room to spare.
void mask_capacity() {
  static_assert(11 <= 16, "11 logical buttons must fit a uint16_t mask");
  constexpr uint16_t kAllMask = 0x07FFu;  // lowest 11 bits set
  assert(kAllMask == 2047);
  for (const M4NavButton b : kAllButtons) {
    const int ord = static_cast<int>(b);
    assert(ord >= 0 && ord < 16);
    assert((kAllMask & static_cast<uint16_t>(1u << ord)) != 0);
  }
  assert((kAllMask & static_cast<uint16_t>(1u << static_cast<int>(M4NavButton::NavNext))) != 0);
  assert((kAllMask & static_cast<uint16_t>(1u << static_cast<int>(M4NavButton::NavPrevious))) != 0);
}

}  // namespace

int main() {
  enum_ordinals();
  member_expansion();
  edge_folds();
  level_folds();
  synth_matcher();
  full_injection_matrix();
  mask_capacity();
  std::cout << "phase1-n1 nav mapping tests passed\n";
  return 0;
}
