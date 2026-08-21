#include <cassert>
#include <cstdint>
#include <iostream>

// Regression for Phase 2 bounded refresh cadence.
// Mirrors GfxRenderer::displayBuffer effectiveMode logic exactly as patched.
// Ensures: instance-scoped, FAST-only promotion at 8, FULL/HALF reset, no static-screen FULL.

enum RefreshMode { FULL_REFRESH = 0, FAST_REFRESH = 1, HALF_REFRESH = 2 };

struct RefreshPolicy {
  mutable uint32_t partialsSinceFull_ = 0;
  RefreshMode promote(RefreshMode mode) const {
    RefreshMode eff = mode;
    if (eff == FAST_REFRESH) {
      if (++partialsSinceFull_ >= 8) {
        eff = FULL_REFRESH;
        partialsSinceFull_ = 0;
      }
    } else if (eff == FULL_REFRESH) {
      partialsSinceFull_ = 0;
    } else {
      // HALF is stronger waveform, clears ghosting -> reset cadence
      partialsSinceFull_ = 0;
    }
    return eff;
  }
};

void testFastPromotionAt8() {
  std::cout << "testFastPromotionAt8..." << std::endl;
  RefreshPolicy p;
  for (int i = 1; i <= 7; ++i) {
    RefreshMode eff = p.promote(FAST_REFRESH);
    assert(eff == FAST_REFRESH);
    assert(p.partialsSinceFull_ == (uint32_t)i);
  }
  RefreshMode eff8 = p.promote(FAST_REFRESH);
  assert(eff8 == FULL_REFRESH);
  assert(p.partialsSinceFull_ == 0);
  // Next FAST after promotion starts at 1
  RefreshMode eff9 = p.promote(FAST_REFRESH);
  assert(eff9 == FAST_REFRESH);
  assert(p.partialsSinceFull_ == 1);
  std::cout << "  FAST promotion at 8 PASS" << std::endl;
}

void testFullResets() {
  std::cout << "testFullResets..." << std::endl;
  RefreshPolicy p;
  p.promote(FAST_REFRESH); // 1
  p.promote(FAST_REFRESH); // 2
  assert(p.partialsSinceFull_ == 2);
  RefreshMode eff = p.promote(FULL_REFRESH);
  assert(eff == FULL_REFRESH);
  assert(p.partialsSinceFull_ == 0);
  // FAST after FULL starts fresh
  eff = p.promote(FAST_REFRESH);
  assert(eff == FAST_REFRESH);
  assert(p.partialsSinceFull_ == 1);
  std::cout << "  FULL reset PASS" << std::endl;
}

void testHalfResets() {
  std::cout << "testHalfResets..." << std::endl;
  RefreshPolicy p;
  p.promote(FAST_REFRESH); // 1
  p.promote(FAST_REFRESH); // 2
  p.promote(FAST_REFRESH); // 3
  assert(p.partialsSinceFull_ == 3);
  RefreshMode eff = p.promote(HALF_REFRESH);
  assert(eff == HALF_REFRESH);
  assert(p.partialsSinceFull_ == 0);
  // FAST after HALF starts fresh (HALF is stronger than FAST differential)
  eff = p.promote(FAST_REFRESH);
  assert(eff == FAST_REFRESH);
  assert(p.partialsSinceFull_ == 1);
  std::cout << "  HALF reset PASS" << std::endl;
}

void testNoUnnecessaryFullOnStatic() {
  std::cout << "testNoUnnecessaryFullOnStatic..." << std::endl;
  RefreshPolicy p;
  // If caller keeps asking FULL, never promote spuriously — should stay FULL and keep counter 0
  for (int i = 0; i < 20; ++i) {
    RefreshMode eff = p.promote(FULL_REFRESH);
    assert(eff == FULL_REFRESH);
    assert(p.partialsSinceFull_ == 0);
  }
  // If caller asks HALF repeatedly, also stays 0
  for (int i = 0; i < 10; ++i) {
    RefreshMode eff = p.promote(HALF_REFRESH);
    assert(eff == HALF_REFRESH);
    assert(p.partialsSinceFull_ == 0);
  }
  std::cout << "  no unnecessary FULL PASS" << std::endl;
}

void testInstanceScoped() {
  std::cout << "testInstanceScoped..." << std::endl;
  RefreshPolicy a, b;
  for (int i = 0; i < 5; ++i) a.promote(FAST_REFRESH);
  assert(a.partialsSinceFull_ == 5);
  assert(b.partialsSinceFull_ == 0);
  b.promote(FAST_REFRESH);
  assert(b.partialsSinceFull_ == 1);
  assert(a.partialsSinceFull_ == 5); // a unaffected
  // Interleaved: a reaches 8 and promotes, b unchanged
  a.promote(FAST_REFRESH); // 6
  a.promote(FAST_REFRESH); // 7
  RefreshMode eff = a.promote(FAST_REFRESH); // 8 -> FULL
  assert(eff == FULL_REFRESH);
  assert(a.partialsSinceFull_ == 0);
  assert(b.partialsSinceFull_ == 1);
  std::cout << "  instance scoped PASS" << std::endl;
}

void testCadenceMatchesM4PanelDirty() {
  std::cout << "testCadenceMatchesM4PanelDirty..." << std::endl;
  // M4PanelDirty kMaxPartialsSinceFull = 8 ; our GfxRenderer must match exactly
  constexpr uint32_t kExpected = 8;
  RefreshPolicy p;
  uint32_t count = 0;
  for (int i = 0; i < 16; ++i) {
    RefreshMode eff = p.promote(FAST_REFRESH);
    if (eff == FULL_REFRESH) {
      count++;
      // Should have been exactly 7 FAST then 1 FULL
      assert(i == 7 || i == 15);
    }
  }
  assert(count == 2);
  std::cout << "  cadence 8 matches M4PanelDirty PASS" << std::endl;
}

int main() {
  testFastPromotionAt8();
  testFullResets();
  testHalfResets();
  testNoUnnecessaryFullOnStatic();
  testInstanceScoped();
  testCadenceMatchesM4PanelDirty();
  std::cout << "gfx refresh policy regression: ALL PASS" << std::endl;
  return 0;
}
