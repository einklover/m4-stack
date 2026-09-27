#pragma once

#include "../ActivityWithSubactivity.h"
#include "apps/M4xInstaller.h"

#include <functional>
#include <string>
#include <vector>

struct AppInstallJob;

// Install .m4x packages (from path or /apps_inbox).
class AppInstallActivity final : public ActivityWithSubactivity {
 public:
  // packagePath: absolute path to .m4x, or empty to pick from inbox.
  explicit AppInstallActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string packagePath,
                              const std::function<void()>& onDone);
  ~AppInstallActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool readyForDestruction() const override;
  bool preventAutoSleep() override;

 private:
  std::string packagePath_;
  std::function<void()> onDone_;
  std::vector<std::string> inboxPackages_;
  int selectedIndex_ = 0;
  M4xInstallResult probe_{};
  enum class Stage { Pick, Confirm, Result } stage_ = Stage::Pick;
  std::string resultMessage_;
  bool updateRequired_ = false;
  // Live until the worker release-stores done. Timeout does not clear this.
  AppInstallJob* job_ = nullptr;
  unsigned long installStartedMs_ = 0;
  bool installTimedOut_ = false;

  void scanInbox();
  void probeSelected();
  void doInstall();
  void observeInstallJob();
  void render() const;
};
