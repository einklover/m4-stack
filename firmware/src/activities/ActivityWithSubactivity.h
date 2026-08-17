#pragma once
#include <memory>

#include "Activity.h"

// Parent owns a single optional child activity.
//
// Lifecycle rule: never destroy the child from inside the child's own
// loop()/callback stack. exitActivity()/enterNewActivity() are safe to call
// from child callbacks: transitions are deferred until subActivity->loop()
// returns to the parent frame.
class ActivityWithSubactivity : public Activity {
 protected:
  std::unique_ptr<Activity> subActivity = nullptr;
  std::unique_ptr<Activity> pendingSubActivity_ = nullptr;
  bool pendingExitSub_ = false;
  bool pumpingSubActivity_ = false;

  void exitActivity();
  void enterNewActivity(Activity* activity);
  // Schedule child teardown after the current child frame returns.
  void requestExitSubActivity() {
    pendingExitSub_ = true;
    pendingSubActivity_.reset();
  }
  bool isExitSubPending() const { return pendingExitSub_; }

 public:
  explicit ActivityWithSubactivity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity(std::move(name), renderer, mappedInput) {}
  void loop() override;
  void onExit() override;

  // Host-testable seam: process one parent frame (child loop + deferred
  // exit/replacement). Returns true if a deferred transition was applied.
  bool pumpSubActivityFrame();
};