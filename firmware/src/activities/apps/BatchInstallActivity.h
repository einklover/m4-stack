#pragma once

#include "../ActivityWithSubactivity.h"

#include <functional>

// Installs every .m4x currently in /apps_inbox. Invalid or failed packages are
// deliberately left in place for inspection/retry; successful and obsolete
// packages are removed by the worker.
class BatchInstallActivity final : public ActivityWithSubactivity {
 public:
  BatchInstallActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                       const std::function<void()>& onDone)
      : ActivityWithSubactivity("BatchInstall", renderer, mappedInput), onDone_(onDone) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

  uint8_t touchFooterButtonsMask() const override {
    return M4FooterTouchPolicy::Back | M4FooterTouchPolicy::Confirm;
  }

 public:
  struct Job;

 private:
  // Visible progress only. total is packages discovered so far, not a finished scan.
  struct ProgressView {
    int total = 0;
    int processed = 0;
    int installed = 0;
    int skipped = 0;
    int failed = 0;
    bool scanning = false;
    bool done = false;
  };

  const std::function<void()> onDone_;
  Job* job_ = nullptr;
  ProgressView painted_{};
  bool hasPaint_ = false;
  unsigned long lastPaintMs_ = 0;

  void render() const;
};
