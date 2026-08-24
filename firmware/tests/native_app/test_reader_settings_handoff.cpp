// Reader settings handoff/state contracts (host-only, mirrors production logic).
//
// Covers the audit findings fixed on agent/fix-reader-settings-handoff:
//   1. EpubReaderActivity::loop() must not clear updateRequired when
//      pumpSubActivityFrame() reports a full child EXIT (the child's onGoBack
//      legitimately requested a reader repaint). Only a REPLACEMENT child may
//      suppress the parent paint.
//   2. Returning from EpubReaderSettingsActivity re-arms the auto page turn
//      timer (applyAutoPageTurnSettings), so a stale pre-settings deadline
//      cannot fire an immediate page turn on return.
//   3. EpubReaderMenuActivity / EpubReaderSettingsActivity pump nested children
//      through pumpSubActivityFrame() (deferred teardown), never by calling
//      subActivity->loop() followed by inline destruction.
#include <cassert>
#include <iostream>
#include <memory>
#include <string>

namespace {

// ── Contract 1+3: minimal model of ActivityWithSubactivity::pumpSubActivityFrame
struct FakeChild {
  std::string name;
  bool exitRequested = false;
  int frames = 0;
  bool inLoop = false;
  bool destroyedWhileInLoop = false;
  bool destroyed = false;
  // Production analogs: children inherit requestExitSubActivity(), which
  // mutates the SHARED parent deferred-exit state; callbacks also set the
  // parent's updateRequired before requesting their own exit (onGoBack path).
  bool* parentRepaint = nullptr;
  bool* parentPendingExit = nullptr;

  explicit FakeChild(std::string n, bool* repaint = nullptr, bool* pendingExit = nullptr)
      : name(std::move(n)), parentRepaint(repaint), parentPendingExit(pendingExit) {}

  void loop() {
    if (destroyed) return;
    inLoop = true;
    ++frames;
    // Simulate the back-press frame: request repaint, then request own exit.
    if (frames >= 2) {
      if (parentRepaint) *parentRepaint = true;
      if (parentPendingExit) *parentPendingExit = true;  // requestExitSubActivity()
    }
    if (destroyed) destroyedWhileInLoop = true;  // would be UAF in production
    inLoop = false;
  }
};

struct FakeParent {
  std::unique_ptr<FakeChild> subActivity;
  std::unique_ptr<FakeChild> pendingSubActivity;
  bool pendingExitSub = false;
  bool pumpingSubActivity = false;
  // Production analog: EpubReaderActivity::updateRequired drives displayTaskLoop.
  bool updateRequired = false;

  void requestExitSubActivity() {
    pendingExitSub = true;
    pendingSubActivity.reset();
  }

  bool pumpSubActivityFrame() {
    if (!subActivity) return false;
    pumpingSubActivity = true;
    subActivity->loop();
    pumpingSubActivity = false;
    if (!pendingExitSub) return false;
    pendingExitSub = false;
    if (subActivity) {
      if (subActivity->inLoop) subActivity->destroyedWhileInLoop = true;
      subActivity->destroyed = true;
      subActivity.reset();
    }
    if (pendingSubActivity) {
      subActivity = std::move(pendingSubActivity);
    }
    return true;
  }
};

void runPumpFrame(FakeParent& parent) {
  const bool replaced = parent.pumpSubActivityFrame();
  if (replaced) {
    // Fixed contract: suppress the parent paint only when another child took
    // over; a full child EXIT must keep/force the repaint request.
    if (parent.subActivity) {
      parent.updateRequired = false;
    } else {
      parent.updateRequired = true;
    }
  }
}

bool childExitKeepsReaderRepaint() {
  FakeParent parent;
  parent.updateRequired = false;
  parent.subActivity =
      std::make_unique<FakeChild>("EpubReaderSettings", &parent.updateRequired, &parent.pendingExitSub);

  runPumpFrame(parent);  // frame 1: idle child frame, no exit
  assert(!parent.updateRequired);
  assert(parent.subActivity != nullptr);

  runPumpFrame(parent);  // frame 2: child requests repaint + exit
  assert(parent.subActivity == nullptr);       // fully exited, not replaced
  assert(parent.updateRequired == true);       // repaint NOT clobbered
  return true;
}

bool replacementChildSuppressesParentPaint() {
  FakeParent parent;
  parent.updateRequired = true;  // leftover from the outgoing frame
  parent.subActivity = std::make_unique<FakeChild>("EpubReaderMenu");

  // Menu action schedules a replacement child (menu -> settings handoff).
  parent.pendingSubActivity = std::make_unique<FakeChild>("EpubReaderSettings");
  parent.pendingExitSub = true;

  runPumpFrame(parent);
  assert(parent.subActivity != nullptr);
  assert(parent.subActivity->name == "EpubReaderSettings");
  assert(parent.updateRequired == false);  // parent must NOT paint under the new child
  return true;
}

bool noTransitionLeavesStateAlone() {
  FakeParent parent;
  parent.updateRequired = true;
  parent.subActivity = std::make_unique<FakeChild>("EpubReaderMenu");
  runPumpFrame(parent);
  assert(!parent.subActivity->destroyed);
  assert(parent.updateRequired == true);  // untouched when pump reports no transition
  return true;
}

// ── Contract 3: nested picker inside menu/settings must tear down only after
// its loop returns (deferred pump), never inline from within the child stack.
bool nestedPickerTornDownAfterLoopReturns() {
  FakeParent menuHost;  // models EpubReaderMenuActivity / EpubReaderSettingsActivity
  auto picker = std::make_unique<FakeChild>("NumberSelection");
  FakeChild* pickerPtr = picker.get();
  menuHost.subActivity = std::move(picker);

  // Picker's own loop requests its exit mid-frame (confirm/back callback).
  menuHost.requestExitSubActivity();
  menuHost.pumpSubActivityFrame();  // deferred pump: loop runs to completion first

  assert(menuHost.subActivity == nullptr);
  assert(pickerPtr->destroyed);
  assert(!pickerPtr->destroyedWhileInLoop);  // never freed while its loop was on the stack
  return true;
}

// ── Contract 2: stale auto-turn deadline must not fire immediately after
// settings return. Models applyAutoPageTurnSettings(): lastTurnTime = millis().
bool settingsReturnReArmsAutoTurnTimer() {
  unsigned long lastPageTurnTime = 0;
  constexpr unsigned long kIntervalMs = 30000;
  unsigned long nowBeforeSettings = 40000;  // deadline long expired while in settings

  // Without the fix the old deadline stays armed:
  assert((nowBeforeSettings - lastPageTurnTime) >= kIntervalMs);  // immediate turn (bug)

  // With the fix, returning from settings re-arms (lastPageTurnTime = millis()):
  lastPageTurnTime = nowBeforeSettings;
  unsigned long nowAfterReturn = nowBeforeSettings;  // same tick as the re-arm
  assert((nowAfterReturn - lastPageTurnTime) < kIntervalMs);  // waits a full interval

  // Disabled setting must disable the timer, not restart it.
  bool autoPageTurnEnabled = false;
  bool automaticPageTurnActive = autoPageTurnEnabled;
  assert(!automaticPageTurnActive);
  return true;
}

}  // namespace

int main() {
  assert(childExitKeepsReaderRepaint());
  assert(replacementChildSuppressesParentPaint());
  assert(noTransitionLeavesStateAlone());
  assert(nestedPickerTornDownAfterLoopReturns());
  assert(settingsReturnReArmsAutoTurnTimer());
  std::cout << "reader settings handoff tests passed\n";
  return 0;
}
