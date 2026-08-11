// PageTurnCoordinator: the production page-turn state machine, extracted so the
// REAL firmware (TxtReaderActivity) and the simulator execute the SAME code —
// no human mirror to drift. This is the fix for the "ReaderModel is a manual
// copy of TxtReaderActivity" credibility gap.
//
// It is hardware-independent: it takes panel-BUSY and index state as inputs and
// produces decisions; it never touches the EPD, SD, or the scheduler. The real
// firmware calls the same methods from its display task; the simulator wires
// the same methods to SimPanel/SimStorage.
//
// Invariants it enforces (the ones that killed real-device bugs):
//   1. pendingPhysicalPage_ snapshots the page at RENDER start. It is what the
//      EPD will show, regardless of how far targetPage advances during BUSY.
//   2. physicalPage is assigned from the COMMITTED frame (provenance), never
//      from live targetPage.
//   3. Catch-up: whenever panel idle, no refresh queued, and target !=
//      physical, the display task must re-arm (and push the index until it
//      covers the target). One valid intent is never left waiting for a
//      second tap.
//
// TestPolicy carries the simulator-only bug knobs used to prove the assertions
// catch regressions. Production code passes the default policy — the bug
// behaviors are unreachable without explicitly opting in.
#pragma once

#include <cstdint>

namespace m4reader {

class PageTurnCoordinator {
public:
  // Test-only bug injection. Production never sets these; they exist so the
  // simulator can prove its temporal assertions catch regressions.
  struct TestPolicy {
    bool bugNoCatchup = false;     // remove catch-up invariant → lost intent
    bool bugLivePhysical = false;  // physicalPage = live target → divergence
    uint32_t quickTapMs = 400;     // taps within this window are "quick"
  };

  // Default policy = production behavior (no injected bugs).
  static TestPolicy productionPolicy() { return TestPolicy{}; }

  explicit PageTurnCoordinator(TestPolicy policy)
      : policy_(policy) {}

  // ── state (read by probes) ──────────────────────────────────────────────
  int targetPage() const { return targetPage_; }
  int physicalPage() const { return physicalPage_; }
  int pendingPhysicalPage() const { return pendingPhysicalPage_; }
  bool updateRequired() const { return updateRequired_; }
  bool firstShown() const { return firstShown_; }
  bool quickMode() const { return quickMode_; }

  void reset() {
    targetPage_ = 0;
    physicalPage_ = -1;
    pendingPhysicalPage_ = -1;
    updateRequired_ = true;  // render first page
    firstShown_ = false;
    quickMode_ = false;
    lastTurnMs_ = 0;
  }

  // ── input path: a page-turn intent ──────────────────────────────────────
  // `panelBusy` is the SSD1677 BUSY state at this instant. Returns the target
  // page after applying the intent (already clamped by the caller).
  void onTap(uint32_t nowMs, bool panelBusy, int newTarget) {
    bool quick = (lastTurnMs_ != 0 && (nowMs - lastTurnMs_ < policy_.quickTapMs)) || panelBusy;
    lastTurnMs_ = nowMs;
    if (quick) {
      quickMode_ = true;
      if (policy_.bugNoCatchup) {
        // BUG: a tap during BUSY cancels the queued refresh (historical root
        // cause of the lost first tap). Correct firmware keeps it armed.
        updateRequired_ = false;
      }
    } else {
      quickMode_ = false;
      updateRequired_ = true;
    }
    targetPage_ = newTarget;
  }

  // ── display task ────────────────────────────────────────────────────────
  // Should the display task render right now? (panel idle + refresh armed)
  bool shouldRender(bool panelBusy, bool suppressDisplay) const {
    if (suppressDisplay) return false;
    if (panelBusy) return false;
    return updateRequired_;
  }

  // Catch-up: with a stable panel and a queued-free but divergent target, the
  // display task MUST re-arm. Returns true when a render must happen.
  bool catchupNeeded(bool panelBusy, bool suppressDisplay) const {
    if (suppressDisplay || panelBusy) return false;
    if (policy_.bugNoCatchup) return false;
    if (!firstShown_) return false;
    if (updateRequired_) return false;
    return targetPage_ != physicalPage_;
  }

  // The target is beyond the current index cursor → keep the refresh armed
  // and push the progressive index until it covers the target.
  bool targetBeyondIndex(bool indexComplete, int indexCursor) const {
    return !indexComplete && targetPage_ >= indexCursor;
  }

  // ── render lifecycle ────────────────────────────────────────────────────
  // Called when rendering STARTS. Snapshots the page the EPD will show and
  // consumes the "render armed" flag (a render is now in flight). Re-arm is
  // the job of the commit callback / catch-up / short-read retry.
  void onRenderStarted() {
    pendingPhysicalPage_ = targetPage_;
    updateRequired_ = false;
  }
  // Called when the render is done and the frame is being handed to the EPD.
  // Returns the page that was laid out (the provenance source).
  int onFrameReady() const { return pendingPhysicalPage_; }

  // ── commit lifecycle ────────────────────────────────────────────────────
  // Called when the SSD1677 BUSY completes: the panel now PHYSICALLY shows
  // `committedPage` (the frame that was submitted). Assigns physicalPage from
  // provenance. Returns true if a decoupled catch-up refresh should be armed
  // (taps accumulated a different target during the animation).
  bool onCommitted(uint32_t /*nowMs*/, int committedPage) {
    if (policy_.bugLivePhysical) {
      // BUG: assigned from live targetPage (advanced during anim).
      physicalPage_ = targetPage_;
    } else {
      physicalPage_ = committedPage;
    }
    firstShown_ = true;
    bool catchup = !policy_.bugNoCatchup && quickMode_ && targetPage_ != physicalPage_;
    if (catchup) updateRequired_ = true;
    quickMode_ = false;
    return catchup;
  }

  // Decoupled catch-up helper: after a commit, if the target moved, render
  // straight to it on the next tick.
  bool needsCatchupRender() const {
    return !policy_.bugNoCatchup && targetPage_ != physicalPage_ && !updateRequired_ &&
           firstShown_;
  }

  // Re-arm a refresh (used by the catch-up invariant and short-read retry).
  void forceCatchupArm() { updateRequired_ = true; }
  void rearm() { updateRequired_ = true; }

  // Test-only: align execution state with the legacy machine during shadow
  // differential (Phase A). updateRequired is consumed mid-tick by the display
  // loop; the probe writes the legacy value so DECISION comparisons are
  // apples-to-apples. Not part of the production contract.
  void setUpdateRequired(bool v) { updateRequired_ = v; }
  void setTarget(int p) { targetPage_ = p; }
  void setPhysical(int p) { physicalPage_ = p; }

private:
  TestPolicy policy_;
  int targetPage_ = 0;
  int physicalPage_ = -1;
  int pendingPhysicalPage_ = -1;
  bool updateRequired_ = true;
  bool firstShown_ = false;
  bool quickMode_ = false;
  uint32_t lastTurnMs_ = 0;
};

}  // namespace m4reader
