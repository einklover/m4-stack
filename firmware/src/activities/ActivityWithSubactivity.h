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

  // m4adb automation seam: expose the actual nested Activity stack instead of
  // forcing E2E tests to infer it from screenshots or timing. This is read-only
  // debug metadata and does not affect production navigation/lifecycle.
  std::string debugUiJson() override {
    std::string out = "{\"subactivity\":\"";
    if (subActivity) out += subActivity->getName();
    out += "\"";
    if (subActivity) {
      std::string child = subActivity->debugUiJson();
      if (child.empty()) child = "{}";
      out += ",\"child\":";
      out += child;
    }
    out += "}";
    return out;
  }

  // Host-testable seam: process one parent frame (child loop + deferred
  // exit/replacement). Returns true if a deferred transition was applied.
  bool pumpSubActivityFrame();
};