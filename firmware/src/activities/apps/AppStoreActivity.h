#pragma once

#include "../ActivityWithSubactivity.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Native lightweight GitHub-backed store. Network and file operations run in
// worker tasks; the screen only consumes completed, immutable copies.
class AppStoreActivity final : public ActivityWithSubactivity {
 public:
  AppStoreActivity(GfxRenderer& renderer, MappedInputManager& input,
                   const std::function<void()>& onBack,
                   const std::function<void()>& onWifiOpen)
      : ActivityWithSubactivity("AppStore", renderer, input),
        onBack_(onBack), onWifiOpen_(onWifiOpen) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool showTouchNavigation() const override { return false; }

 private:
  struct App {
    std::string id, name, version, description, category, packageUrl, sha256, sourceUrl;
    int versionCode = 0;
  };
  struct State;
  struct Job;
  std::shared_ptr<State> state_;
  std::function<void()> onBack_;
  std::function<void()> onWifiOpen_;
  std::vector<App> shown_;
  std::vector<std::pair<std::string,int>> installed_;
  int selected_ = 0;
  int page_ = 0;
  bool detail_ = false;
  bool dirty_ = true;
  bool handoff_ = false;
  uint32_t seenRevision_ = 0;
  std::string status_;
  bool offline_ = false;

  static void taskEntry(void* arg);
  void startJob(bool download, const App* app = nullptr);
  void refreshSnapshot();
  void render();
  void select();
};
