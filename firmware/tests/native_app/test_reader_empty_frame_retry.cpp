// RED/GREEN host contract: the reader's empty-first-frame retry must be a
// bounded, terminal policy.
//
// Bug (device): entering a provider 正文 whose chapter cache lays out zero
// renderable lines (whitespace-only body that passes FanqieProvider's only
// guard `file.written() == 0`, or a persistent read-window alloc failure right
// after handoff) made TxtReaderActivity::displayTaskLoop re-arm
// cachedPage=-1 + updateRequired on EVERY ~10ms tick forever: SD re-read +
// full word-wrap + serial spam at ~100Hz while the screen stays frozen on the
// entry placeholder. The old code comment itself admitted "the retry spins
// forever on the same empty lines". This test pins the bounded policy the
// display loop must now use.
#include "util/M4TxtIndexPolicy.h"

#include <cassert>
#include <cstdio>

namespace {

// Model of the displayTaskLoop empty-frame branch using the production policy.
// Returns the number of ticks until the loop goes quiet (terminal paint shown,
// no more updateRequired re-arms). A budget of "never terminates" would make
// this function run forever — exactly the device freeze.
int simulateEmptyFrameLoop(int maxTicks /*safety harness bound*/) {
  int consecutive = 0;
  for (int tick = 0; tick < maxTicks; ++tick) {
    ++consecutive;
    if (M4TxtIndexPolicy::emptyFirstFrameShouldRetry(consecutive)) {
      continue;  // re-arm updateRequired, reload next tick
    }
    return tick + 1;  // terminal: paint once, stop spinning
  }
  return -1;  // still spinning at harness bound == RED (unbounded retry)
}

void testBudgetIsBounded() {
  // Must terminate. Generous enough for transient alloc/index states
  // (~10ms tick ⇒ seconds of grace), small enough to actually stop.
  constexpr int kMaxTicks = 100000;
  const int ticks = simulateEmptyFrameLoop(kMaxTicks);
  assert(ticks > 0);
  assert(ticks <= kMaxTicks);
}

void testBudgetGivesTransientRecoveryAGraceWindow() {
  // Transient states (one-shot alloc failure, index catch-up between windows)
  // must still recover: retries are allowed inside the budget.
  assert(M4TxtIndexPolicy::emptyFirstFrameShouldRetry(1));
  assert(M4TxtIndexPolicy::emptyFirstFrameShouldRetry(
      M4TxtIndexPolicy::kEmptyFirstFrameMaxRetries));
}

void testBudgetIsTerminalBeyondLimit() {
  // Past the budget the loop must NOT re-arm again.
  assert(!M4TxtIndexPolicy::emptyFirstFrameShouldRetry(
      M4TxtIndexPolicy::kEmptyFirstFrameMaxRetries + 1));
  assert(!M4TxtIndexPolicy::emptyFirstFrameShouldRetry(1000000));
}

void testBudgetIsSmallEnoughToStopTheSpin() {
  // The whole point: an unrenderable chapter must reach terminal within
  // seconds of 10ms ticks, not minutes.
  assert(M4TxtIndexPolicy::kEmptyFirstFrameMaxRetries > 0);
  assert(M4TxtIndexPolicy::kEmptyFirstFrameMaxRetries <= 120);
}

}  // namespace

int main() {
  testBudgetIsBounded();
  testBudgetGivesTransientRecoveryAGraceWindow();
  testBudgetIsTerminalBeyondLimit();
  testBudgetIsSmallEnoughToStopTheSpin();
  std::printf("reader empty-frame retry contract: PASS\n");
  return 0;
}
