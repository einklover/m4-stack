#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <memory>
#include <string>

#include "NetworkModeSelectionActivity.h"
#include "activities/ActivityWithSubactivity.h"
#include "navigation/M4NavigationSupervisor.h"
#include "network/M4FileTransferService.h"

enum class WebServerActivityState {
  MODE_SELECTION,
  WIFI_SELECTION,
  AP_STARTING,
  SERVER_RUNNING,
  ERROR,
  SHUTTING_DOWN
};

/** File-transfer entry activity: mode selection → network setup → server. */
class CrossPointWebServerActivity final : public ActivityWithSubactivity {
  enum class PendingParentAction : uint8_t { None, StartAccessPoint, EnterWifiSelection, StartWebServer };

  struct DeferredCleanupContext {
    M4FileTransferService service;
    bool isApMode = false;
  };

  TaskHandle_t displayTaskHandle = nullptr;
  TaskHandle_t deferredCleanupTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;
  WebServerActivityState state = WebServerActivityState::MODE_SELECTION;
  const std::function<void()> onGoBack;

  NetworkMode networkMode = NetworkMode::JOIN_NETWORK;
  bool isApMode = false;
  std::unique_ptr<DeferredCleanupContext> deferredCleanupContext;
  M4NavigationSupervisor navigationSupervisor;
  std::string connectedIP;
  std::string connectedSSID;
  std::string setupError;
  bool autoStartSavedSta = false;
  unsigned long lastHandleClientTime = 0;
  PendingParentAction pendingParentAction = PendingParentAction::None;

  static void taskTrampoline(void* param);
  static void deferredCleanupTaskTrampoline(void* param);
  static bool webPumpAbortCheck(void* context);
  bool pollWebPumpAbort();
  [[noreturn]] void displayTaskLoop();
  void render() const;
  void renderServerRunning() const;
  void showSetupError(const char* message);
  void reopenModeSelection();
  void runPendingParentAction();
  void requestNavigationExit();
  bool ensureDeferredCleanupWorker();
  M4FileTransferService& fileTransferService() { return deferredCleanupContext->service; }
  const M4FileTransferService& fileTransferService() const { return deferredCleanupContext->service; }

  void onNetworkModeSelected(NetworkMode mode);
  void onWifiSelectionComplete(bool connected);
  void startAccessPoint();
  void startWebServer();

 public:
  explicit CrossPointWebServerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       const std::function<void()>& onGoBack, bool autoStart = false)
      : ActivityWithSubactivity("CrossPointWebServer", renderer, mappedInput), onGoBack(onGoBack),
        deferredCleanupContext(new DeferredCleanupContext()), autoStartSavedSta(autoStart) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  std::string debugUiJson() override {
    std::string out = "{\"subactivity\":\"";
    if (subActivity) out += subActivity->getName();
    out += "\"}";
    return out;
  }
  bool skipLoopDelay() override {
#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
    return false;
#else
    return deferredCleanupContext && deferredCleanupContext->service.webServerRunning();
#endif
  }
  bool preventAutoSleep() override {
    return deferredCleanupContext && deferredCleanupContext->service.webServerRunning();
  }
};