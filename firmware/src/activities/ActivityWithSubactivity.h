#pragma once
#include <memory>

#include "Activity.h"

// Parent owns a single optional child activity.
//
// Lifecycle rule: never destroy the child from inside the child's own
// loop()/callback stack. Prefer requestExitSubActivity() so the parent
// deletes the child only after subActivity->loop() has returned.
class ActivityWithSubactivity : public Activity {
 protected:
  std::unique_ptr<Activity> subActivity = nullptr;
  bool pendingExitSub_ = false;

  void exitActivity();
  void enterNewActivity(Activity* activity);
  // Schedule child teardown after the current child frame returns.
  void requestExitSubActivity() { pendingExitSub_ = true; }
  bool isExitSubPending() const { return pendingExitSub_; }

 public:
  explicit ActivityWithSubactivity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity(std::move(name), renderer, mappedInput) {}
  void loop() override;
  void onExit() override;

  // Host-testable seam: process one parent frame (child loop + deferred exit).
  // Returns true if a deferred exit was applied this frame.
  bool pumpSubActivityFrame();
};
