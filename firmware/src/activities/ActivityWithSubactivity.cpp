#include "ActivityWithSubactivity.h"

#include "util/M4FooterTouchPolicy.h"
#include "util/M4ReaderFrontlightGesture.h"
#include "util/M4TouchNavigation.h"
#include "util/M4UiRuntimePolicy.h"

namespace {
void restoreParentPolicies(ActivityWithSubactivity& activity) {
  M4TouchNavigation::activateForActivity(activity.showTouchNavigation());
  M4UiRuntimePolicy::setTextScalePercent(activity.uiTextScalePercent());
  M4ReaderFrontlightGesture::setEnabled(activity.isReaderBodyActivity());
  M4FooterTouchPolicy::setMask(activity.touchFooterButtonsMask());
}
}  // namespace

void ActivityWithSubactivity::exitActivity() {
  // Child callbacks run inside subActivity->loop(). Destroying that child here
  // is a use-after-free as soon as the callback returns. Defer teardown until
  // pumpSubActivityFrame() regains control.
  if (pumpingSubActivity_) {
    pendingExitSub_ = true;
    pendingSubActivity_.reset();
    return;
  }

  if (subActivity) {
    subActivity->onExit();
    subActivity.reset();
  }
  pendingExitSub_ = false;
  pendingSubActivity_.reset();
  restoreParentPolicies(*this);
}

void ActivityWithSubactivity::enterNewActivity(Activity* activity) {
  if (activity) activity->setParentActivity(this);

  if (pumpingSubActivity_) {
    // Replace only after the currently executing child frame returns. Ownership
    // is transferred immediately so callers do not need a separate pending
    // object lifetime.
    pendingExitSub_ = true;
    pendingSubActivity_.reset(activity);
    return;
  }

  exitActivity();
  subActivity.reset(activity);
  if (subActivity) subActivity->onEnter();
}

bool ActivityWithSubactivity::pumpSubActivityFrame() {
  if (!subActivity) return false;

  pumpingSubActivity_ = true;
  subActivity->loop();
  pumpingSubActivity_ = false;

  if (!pendingExitSub_) return false;

  pendingExitSub_ = false;
  if (subActivity) {
    subActivity->onExit();
    subActivity.reset();
  }
  restoreParentPolicies(*this);

  if (pendingSubActivity_) {
    subActivity = std::move(pendingSubActivity_);
    subActivity->onEnter();
  }
  return true;
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
  if (pendingSubActivity_) {
    pendingSubActivity_.reset();
  }
  pendingExitSub_ = false;
  pumpingSubActivity_ = false;
}