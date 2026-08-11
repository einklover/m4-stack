#pragma once

#include "../ActivityWithSubactivity.h"
#include "apps/M4PluginReaderSession.h"
#include "apps/M4xLuaHost.h"
#include "apps/M4xRegistry.h"
#include "apps/M4xRuntimePump.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <functional>
#include <string>

// All Lua entry runs on one owner FreeRTOS task. UI only posts events and
// waits on OwnerLifecycle::ownerDone before freeing queue/mutex / destroying
// this activity. Native TxtReaderActivity opens as a sub-activity; child is
// never deleted from inside its own loop (two-phase close).
class AppRuntimeActivity final : public ActivityWithSubactivity {
 public:
  explicit AppRuntimeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, M4xInstalledApp app,
                              const std::function<void()>& onExitApp);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  // m4adb `ui`: failed/error + Lua screen/status/message + host list scene.
  std::string debugUiJson() override;

 private:
  M4xInstalledApp app_;
  std::function<void()> onExitApp_;
  M4xLuaHost host_;

  M4xRuntime::OwnerLifecycle life_;

  std::atomic<bool> failed_{false};
  std::atomic<bool> exitRequested_{false};
  std::atomic<bool> ready_{false};
  char errorBuf_[160] = {};
  SemaphoreHandle_t errorMutex_ = nullptr;

  TaskHandle_t runtimeTask_ = nullptr;
  QueueHandle_t eventQueue_ = nullptr;
  bool ownerTaskStarted_ = false;

  // Set by child onGoBack; applied only after subActivity->loop() returns.
  bool childClosePending_ = false;
  // Which native plugin child is active (reader vs system TOC).
  enum class PluginChildKind : uint8_t { None, Reader, Toc };
  PluginChildKind pluginChildKind_ = PluginChildKind::None;
  // Sticky meta for TOC close when child is torn down.
  std::string tocBookId_;
  uint32_t tocGeneration_ = 0;

  static void taskTrampoline(void* param);
  void runtimeTaskMain();
  bool postEvent(const M4xRuntime::Event& e);
  void handleEventOnOwner(const M4xRuntime::Event& e);
  void setFailed(const std::string& err);
  void copyError(std::string& out);
  void renderError();
  void requestStopAndJoin();
  void tryLaunchPluginReader();
  void tryLaunchPluginToc();
};
