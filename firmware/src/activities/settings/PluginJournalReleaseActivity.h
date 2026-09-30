#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "activities/ActivityWithSubactivity.h"
#include "activities/settings/M4PluginJournalReleaseUi.h"

// Settings → 维护 → 修复未完成插件安装记录.
// Lists pending journal ids (including ids absent from the registry) and calls
// M4xInstaller::archiveAndReleasePending only after an explicit confirm.
class PluginJournalReleaseActivity final : public ActivityWithSubactivity {
 public:
  explicit PluginJournalReleaseActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        const std::function<void()>& goBack)
      : ActivityWithSubactivity("PluginJournalRelease", renderer, mappedInput), goBack_(goBack) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  struct Row {
    std::string id;
    bool inRegistry = false;
  };

  TaskHandle_t displayTaskHandle_ = nullptr;
  SemaphoreHandle_t renderingMutex_ = nullptr;
  bool updateRequired_ = false;
  bool listOk_ = false;
  bool truncated_ = false;
  std::string status_;
  std::vector<Row> rows_;
  M4JournalReleaseUi ui_;
  const std::function<void()> goBack_;

  static void taskTrampoline(void* param);
  void displayTaskLoop();
  void render();
  void reloadRows();
  void applyRelease();
};
