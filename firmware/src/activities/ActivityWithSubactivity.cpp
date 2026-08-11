#include "ActivityWithSubactivity.h"

#include "util/M4FooterTouchPolicy.h"
#include "util/M4ReaderFrontlightGesture.h"
#include "util/M4TouchNavigation.h"
#include "util/M4UiRuntimePolicy.h"

void ActivityWithSubactivity::exitActivity() {
  if (subActivity) {
    subActivity->onExit();
    subActivity.reset();
  }
  pendingExitSub_ = false;
  // Child onExit() disables global touch chrome, reader side-light gestures,
  // activity-owned footer touch, and restores the default text scale. Restore
  // all parent policies so the parent returns exactly as it was.
  M4TouchNavigation::activateForActivity(showTouchNavigation());
  M4UiRuntimePolicy::setTextScalePercent(uiTextScalePercent());
  M4ReaderFrontlightGesture::setEnabled(isReaderBodyActivity());
  M4FooterTouchPolicy::setMask(touchFooterButtonsMask());
}

void ActivityWithSubactivity::enterNewActivity(Activity* activity) {
  // Replace any existing child safely (caller must not be inside child loop).
  exitActivity();
  subActivity.reset(activity);
  if (subActivity) subActivity->onEnter();
}

bool ActivityWithSubactivity::pumpSubActivityFrame() {
  if (!subActivity) return false;
  subActivity->loop();
  if (pendingExitSub_) {
    pendingExitSub_ = false;
    exitActivity();
    return true;
  }
  return false;
}

void ActivityWithSubactivity::loop() {
  if (subActivity) {
    pumpSubActivityFrame();
  }
}

void ActivityWithSubactivity::onExit() {
  Activity::onExit();
  // Only call onExit on subActivity, do NOT reset (free) it here.
  // The destructor will handle cleanup. This prevents use-after-free when
  // onExit is triggered from within subActivity's own call stack
  // (e.g., reader back button → goToLibrary → global::exitActivity).
  if (subActivity) {
    subActivity->onExit();
  }
}