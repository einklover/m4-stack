#pragma once

#include "../ActivityWithSubactivity.h"
#include "apps/M4xInstaller.h"

#include <functional>
#include <string>
#include <vector>

// Install .m4x packages (from path or /apps_inbox).
class AppInstallActivity final : public ActivityWithSubactivity {
 public:
  // packagePath: absolute path to .m4x, or empty to pick from inbox.
  explicit AppInstallActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string packagePath,
                              const std::function<void()>& onDone);

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  std::string packagePath_;
  std::function<void()> onDone_;
  std::vector<std::string> inboxPackages_;
  int selectedIndex_ = 0;
  M4xInstallResult probe_{};
  enum class Stage { Pick, Confirm, Result } stage_ = Stage::Pick;
  std::string resultMessage_;
  bool updateRequired_ = false;
  bool installRunning_ = false;

  TaskHandle_t displayTaskHandle_ = nullptr;
  SemaphoreHandle_t renderingMutex_ = nullptr;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void scanInbox();
  void probeSelected();
  void doInstall();
  void render() const;
};
