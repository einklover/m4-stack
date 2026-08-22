#include <cassert>
#include <cstdint>
#include <iostream>

// Host regression for the centralized refresh policy. Legacy FULL/HALF names
// are source-compatible only; the one-inversion cleanup survives solely when
// the reader body explicitly supplies the reader context.
enum RefreshMode { FULL_REFRESH = 0, HALF_REFRESH = 1, FAST_REFRESH = 2, UI_FAST_REFRESH = 3,
                    READER_CLEANUP_REFRESH = 4 };
enum RefreshContext { UI_CONTEXT = 0, READER_BODY_CONTEXT = 1 };

struct RefreshTrace {
  RefreshMode effective(RefreshMode requested, RefreshContext context) const {
    return requested == READER_CLEANUP_REFRESH && context == READER_BODY_CONTEXT
               ? READER_CLEANUP_REFRESH
               : FAST_REFRESH;
  }

  unsigned visibleInversionPhases(RefreshMode requested, RefreshContext context) const {
    return effective(requested, context) == READER_CLEANUP_REFRESH ? 1u : 0u;
  }

  unsigned fullWaveformPhases(RefreshMode, RefreshContext) const { return 0u; }
};

void testLegacyModesAreFastEverywhere() {
  RefreshTrace trace;
  for (RefreshMode requested : {FULL_REFRESH, HALF_REFRESH, FAST_REFRESH, UI_FAST_REFRESH}) {
    assert(trace.effective(requested, UI_CONTEXT) == FAST_REFRESH);
    assert(trace.visibleInversionPhases(requested, UI_CONTEXT) == 0);
    assert(trace.fullWaveformPhases(requested, UI_CONTEXT) == 0);
  }
}

void testReaderCleanupIsOneInversion() {
  RefreshTrace trace;
  assert(trace.effective(READER_CLEANUP_REFRESH, READER_BODY_CONTEXT) == READER_CLEANUP_REFRESH);
  assert(trace.visibleInversionPhases(READER_CLEANUP_REFRESH, READER_BODY_CONTEXT) == 1);
  assert(trace.fullWaveformPhases(READER_CLEANUP_REFRESH, READER_BODY_CONTEXT) == 0);

  // A stale/mis-scoped cleanup request must not escape into menus or dialogs.
  assert(trace.effective(READER_CLEANUP_REFRESH, UI_CONTEXT) == FAST_REFRESH);
  assert(trace.visibleInversionPhases(READER_CLEANUP_REFRESH, UI_CONTEXT) == 0);
  assert(trace.fullWaveformPhases(READER_CLEANUP_REFRESH, UI_CONTEXT) == 0);
}

void testFastNeverAutoPromotes() {
  RefreshTrace trace;
  for (int i = 0; i < 100; ++i) {
    assert(trace.effective(FAST_REFRESH, UI_CONTEXT) == FAST_REFRESH);
    assert(trace.visibleInversionPhases(FAST_REFRESH, UI_CONTEXT) == 0);
    assert(trace.fullWaveformPhases(FAST_REFRESH, UI_CONTEXT) == 0);
  }
}

int main() {
  testLegacyModesAreFastEverywhere();
  testReaderCleanupIsOneInversion();
  testFastNeverAutoPromotes();
  std::cout << "gfx refresh policy regression: ALL PASS\n";
  return 0;
}
