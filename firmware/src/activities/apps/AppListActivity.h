#pragma once

#include "../ActivityWithSubactivity.h"
#include "apps/M4xRegistry.h"

#include <functional>
#include <string>
#include <vector>

// Installed extension apps (APK-like drawer).
class AppListActivity final : public ActivityWithSubactivity {
 public:
  explicit AppListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                           const std::function<void()>& onGoBack);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool showTouchNavigation() const override { return false; }
  uint8_t touchFooterButtonsMask() const override {
    return mode_ == 1 ? M4FooterTouchPolicy::Back | M4FooterTouchPolicy::Confirm
                      : M4FooterTouchPolicy::Back | M4FooterTouchPolicy::Confirm |
                            M4FooterTouchPolicy::Left | M4FooterTouchPolicy::Right;
  }

 private:
  std::function<void()> onGoBack;
  std::vector<M4xInstalledApp> apps_;
  int selectedIndex_ = 0;
  bool updateRequired_ = false;
  // 0 = list mode, 1 = confirm uninstall
  int mode_ = 0;
  bool uninstallClearData_ = true;

  TaskHandle_t displayTaskHandle_ = nullptr;
  SemaphoreHandle_t renderingMutex_ = nullptr;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void reload();
  void render() const;
  void openSelected();
  void openInstall();
  void uninstallSelected();
};
