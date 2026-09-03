#include "CrossPointWebServerActivity.h"

#include <GfxRenderer.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include <cstddef>

#include "MappedInputManager.h"
#include "I18n.h"
#include "NetworkModeSelectionActivity.h"
#include "WifiSelectionActivity.h"
#include "activities/network/CalibreConnectActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"
#include "network/NetworkConstants.h"
#include "qemu/M4QemuNet.h"
#include "util/M4RuntimeMemory.h"
#include "util/QRCodeHelper.h"

using namespace NetworkConstants;

namespace {
void logInternalHeap(const char* where) {
  Serial.printf("[%lu] [WEBACT] [MEM] %s internal_free=%u largest=%u total_heap=%u\n", millis(), where,
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                static_cast<unsigned>(ESP.getFreeHeap()));
}

void logNavigationRequest(const WebServerActivityState state, const bool isApMode) {
  Serial.printf("[%lu] [WEBACT] nav_request state=%u mode=%s\n", millis(), static_cast<unsigned>(state),
                isApMode ? "ap" : "sta");
  m4LogRuntimeMemory("file-transfer-nav-request");
}
}  // namespace

void CrossPointWebServerActivity::taskTrampoline(void* param) {
  static_cast<CrossPointWebServerActivity*>(param)->displayTaskLoop();
}

void CrossPointWebServerActivity::deferredCleanupTaskTrampoline(void* param) {
  auto* cleanup = static_cast<DeferredCleanupContext*>(param);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  if (cleanup) {
    const bool cleanupApMode = cleanup->isApMode;
    const unsigned long cleanupStarted = millis();
    cleanup->service.stop(cleanupApMode);
    Serial.printf("[%lu] [WEBACT] deferred_cleanup_ms=%lu mode=%s\n", millis(),
                  static_cast<unsigned long>(millis() - cleanupStarted), cleanupApMode ? "ap" : "sta");
    delete cleanup;
  }
  vTaskDelete(nullptr);
}

bool CrossPointWebServerActivity::webPumpAbortCheck(void* context) {
  return static_cast<CrossPointWebServerActivity*>(context)->pollWebPumpAbort();
}

bool CrossPointWebServerActivity::pollWebPumpAbort() {
  if (!navigationSupervisor.canDispatchCallback()) return false;
  mappedInput.update();
  return mappedInput.wasPressed(MappedInputManager::Button::Back);
}

bool CrossPointWebServerActivity::ensureDeferredCleanupWorker() {
  if (deferredCleanupTaskHandle) return true;
  if (!deferredCleanupContext) return false;
  return xTaskCreate(&CrossPointWebServerActivity::deferredCleanupTaskTrampoline, "WebServerCleanup", 4096,
                     deferredCleanupContext.get(), 1, &deferredCleanupTaskHandle) == pdPASS;
}

void CrossPointWebServerActivity::requestNavigationExit() {
  if (state == WebServerActivityState::SHUTTING_DOWN) return;
  navigationSupervisor.detach();
  pendingParentAction = PendingParentAction::None;
  state = WebServerActivityState::SHUTTING_DOWN;
  updateRequired = false;
  onGoBack();
}

void CrossPointWebServerActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  m4LogRuntimeMemory("file-transfer-enter");
  logInternalHeap("onEnter");

  if (!deferredCleanupContext) deferredCleanupContext.reset(new DeferredCleanupContext());
  deferredCleanupTaskHandle = nullptr;
  navigationSupervisor.detach();
  renderingMutex = xSemaphoreCreateMutex();
  state = WebServerActivityState::MODE_SELECTION;
  networkMode = NetworkMode::JOIN_NETWORK;
  isApMode = false;
  connectedIP.clear();
  connectedSSID.clear();
  setupError.clear();
  lastHandleClientTime = 0;
  pendingParentAction = PendingParentAction::None;
  updateRequired = true;

  // Server-running rendering builds QR codes with a 512-byte scratch buffer in
  // addition to the text/rendering call stack. Match the other network display
  // tasks: 2KB is not enough for this path and trips the FreeRTOS stack canary.
  xTaskCreate(&CrossPointWebServerActivity::taskTrampoline, "WebServerActivityTask", 4096, this, 1,
              &displayTaskHandle);

  if (autoStartSavedSta && M4QemuNet::staConnected()) {
    isApMode = false;
    connectedIP = M4QemuNet::localIpStd();
    connectedSSID = M4QemuNet::ssidStd();
    if (connectedIP.empty() && m4QemuNetWifiCompatConnected()) connectedIP = "10.0.2.15";
    if (connectedSSID.empty() && m4QemuNetWifiCompatConnected()) connectedSSID = "qemu-openeth";
    fileTransferService().beginStationMdns(AP_HOSTNAME);
    startWebServer();
    return;
  }

  reopenModeSelection();
}

void CrossPointWebServerActivity::onExit() {
  const unsigned long exitStarted = millis();
  Serial.printf("[%lu] [WEBACT] exit_begin state=%u mode=%s\n", millis(), static_cast<unsigned>(state),
                isApMode ? "ap" : "sta");
  m4LogRuntimeMemory("file-transfer-exit-begin");

  navigationSupervisor.detach();
  ActivityWithSubactivity::onExit();
  pendingParentAction = PendingParentAction::None;
  state = WebServerActivityState::SHUTTING_DOWN;
  updateRequired = false;

  bool renderMutexHeld = false;
  unsigned long renderMutexWaitMs = 0;
  if (renderingMutex) {
    const unsigned long mutexWaitStarted = millis();
    renderMutexHeld = xSemaphoreTake(renderingMutex, pdMS_TO_TICKS(25)) == pdTRUE;
    renderMutexWaitMs = static_cast<unsigned long>(millis() - mutexWaitStarted);
  }
  Serial.printf("[%lu] [WEBACT] render_mutex_wait_ms=%lu acquired=%d\n", millis(), renderMutexWaitMs,
                renderMutexHeld ? 1 : 0);

  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  if (renderMutexHeld && renderingMutex) xSemaphoreGive(renderingMutex);
  if (renderingMutex) {
    vSemaphoreDelete(renderingMutex);
    renderingMutex = nullptr;
  }

  bool cleanupDeferred = false;
  if (deferredCleanupContext) {
    if (!deferredCleanupTaskHandle) ensureDeferredCleanupWorker();
    if (deferredCleanupTaskHandle) {
      deferredCleanupContext->isApMode = isApMode;
      deferredCleanupContext.release();
      TaskHandle_t cleanupTask = deferredCleanupTaskHandle;
      deferredCleanupTaskHandle = nullptr;
      xTaskNotifyGive(cleanupTask);
      cleanupDeferred = true;
    }
  }
  Serial.printf("[%lu] [WEBACT] cleanup_deferred=%d\n", millis(), cleanupDeferred ? 1 : 0);

  m4LogRuntimeMemory("file-transfer-exit-end");
  Serial.printf("[%lu] [WEBACT] exit_ms=%lu\n", millis(), static_cast<unsigned long>(millis() - exitStarted));
}

void CrossPointWebServerActivity::showSetupError(const char* message) {
  navigationSupervisor.detach();
  fileTransferService().stopForSetupError(isApMode);
  pendingParentAction = PendingParentAction::None;
  setupError = message ? message : "Network setup failed";
  state = WebServerActivityState::ERROR;
  updateRequired = true;
  logInternalHeap(setupError.c_str());
}

void CrossPointWebServerActivity::reopenModeSelection() {
  navigationSupervisor.detach();
  pendingParentAction = PendingParentAction::None;
  setupError.clear();
  state = WebServerActivityState::MODE_SELECTION;
  enterNewActivity(new NetworkModeSelectionActivity(
      renderer, mappedInput, [this](const NetworkMode mode) { onNetworkModeSelected(mode); },
      [this]() { requestNavigationExit(); }));
}

void CrossPointWebServerActivity::runPendingParentAction() {
  if (subActivity || pendingParentAction == PendingParentAction::None) return;
  const PendingParentAction action = pendingParentAction;
  pendingParentAction = PendingParentAction::None;

  switch (action) {
    case PendingParentAction::StartAccessPoint:
      state = WebServerActivityState::AP_STARTING;
      updateRequired = true;
      startAccessPoint();
      return;
    case PendingParentAction::EnterWifiSelection:
      WiFi.mode(WIFI_STA);
      state = WebServerActivityState::WIFI_SELECTION;
      enterNewActivity(new WifiSelectionActivity(
          renderer, mappedInput, [this](const bool connected) { onWifiSelectionComplete(connected); }));
      return;
    case PendingParentAction::StartWebServer:
      fileTransferService().beginStationMdns(AP_HOSTNAME);
      startWebServer();
      return;
    case PendingParentAction::None:
      return;
  }
}

void CrossPointWebServerActivity::onNetworkModeSelected(const NetworkMode mode) {
  networkMode = mode;
  isApMode = mode == NetworkMode::CREATE_HOTSPOT;

  if (mode == NetworkMode::CONNECT_CALIBRE) {
    // enterNewActivity() is deferred by ActivityWithSubactivity while the mode
    // child's callback frame is active. Calibre::onEnter therefore happens only
    // after NetworkModeSelection's 4KB display task has been destroyed.
    enterNewActivity(new CalibreConnectActivity(renderer, mappedInput, [this] {
      exitActivity();
      reopenModeSelection();
    }));
    return;
  }

  // Do not initialize WiFi/AP/HTTP while NetworkModeSelection is still alive.
  // Its display task uses a 4096-byte stack from the same internal heap needed
  // by WiFi/LWIP. Record one byte of intent and execute it after the safe child
  // pump has completed the child's onExit().
  exitActivity();

  if (mode == NetworkMode::JOIN_NETWORK) {
#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
    if (m4QemuNetWifiCompatConnected()) {
      isApMode = false;
      connectedIP = M4QemuNet::localIpStd();
      connectedSSID = M4QemuNet::ssidStd();
      if (connectedIP.empty()) connectedIP = "10.0.2.15";
      if (connectedSSID.empty()) connectedSSID = "qemu-openeth";
      pendingParentAction = PendingParentAction::StartWebServer;
      return;
    }
#endif
    pendingParentAction = PendingParentAction::EnterWifiSelection;
    return;
  }

#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
  if (m4QemuNetWifiCompatConnected()) {
    isApMode = false;
    connectedIP = M4QemuNet::localIpStd();
    connectedSSID = M4QemuNet::ssidStd();
    if (connectedIP.empty()) connectedIP = "10.0.2.15";
    if (connectedSSID.empty()) connectedSSID = "qemu-openeth";
    pendingParentAction = PendingParentAction::StartWebServer;
    return;
  }
#endif

  pendingParentAction = PendingParentAction::StartAccessPoint;
}

void CrossPointWebServerActivity::onWifiSelectionComplete(const bool connected) {
  if (connected) {
    // Read child-owned data before requesting its deferred teardown, but defer
    // HTTP/mDNS startup until the child's display task has actually been freed.
    if (subActivity) {
      connectedIP = static_cast<WifiSelectionActivity*>(subActivity.get())->getConnectedIP();
    }
    if (connectedIP.empty() || connectedIP == "0.0.0.0") connectedIP = M4QemuNet::localIpStd();
    connectedSSID = M4QemuNet::ssidStd();
    if (connectedSSID.empty()) connectedSSID = WiFi.SSID().c_str();
    isApMode = false;
    exitActivity();
    pendingParentAction = PendingParentAction::StartWebServer;
  } else {
    exitActivity();
    reopenModeSelection();
  }
}

void CrossPointWebServerActivity::startAccessPoint() {
  logInternalHeap("before AP start (selection child released)");
  if (!fileTransferService().beginAccessPoint(AP_SSID, AP_PASSWORD, AP_CHANNEL, AP_MAX_CONNECTIONS, AP_HOSTNAME,
                                              connectedIP)) {
    showSetupError("Hotspot startup failed");
    return;
  }
  connectedSSID = AP_SSID;
  logInternalHeap("after AP start");
  startWebServer();
}

void CrossPointWebServerActivity::startWebServer() {
  m4LogRuntimeMemory("file-transfer-server-start-before");
  logInternalHeap("before web server start");
  if (!ensureDeferredCleanupWorker()) {
    showSetupError("Cleanup worker memory allocation failed");
    return;
  }
  const auto result = fileTransferService().beginWebServer();
  if (result == M4FileTransferService::WebServerStartResult::AllocationFailed) {
    showSetupError("Web server memory allocation failed");
    return;
  }
  if (result == M4FileTransferService::WebServerStartResult::StartupFailed) {
    showSetupError("Web server startup failed");
    return;
  }
  setupError.clear();
  navigationSupervisor.attach();
  state = WebServerActivityState::SERVER_RUNNING;
  updateRequired = true;
  logInternalHeap("after web server start");
  m4LogRuntimeMemory("file-transfer-server-start-after");

  if (renderingMutex) xSemaphoreTake(renderingMutex, portMAX_DELAY);
  render();
  if (renderingMutex) xSemaphoreGive(renderingMutex);
}

void CrossPointWebServerActivity::loop() {
  if (subActivity) {
    pumpSubActivityFrame();
    if (!subActivity) runPendingParentAction();
    return;
  }

  runPendingParentAction();
  if (subActivity) return;

  if (state == WebServerActivityState::SHUTTING_DOWN) return;

  auto wantsExit = [this]() -> bool {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) return true;
    if (!mappedInput.hasTouch()) return false;
    if (mappedInput.wasBackGesture() || mappedInput.wasHomeGesture()) return true;
    if (mappedInput.wasSwipe() == MappedInputManager::SwipeDir::Left) return true;
    return false;
  };

  if (wantsExit()) {
    if (state == WebServerActivityState::ERROR) {
      reopenModeSelection();
    } else {
      logNavigationRequest(state, isApMode);
      requestNavigationExit();
    }
    return;
  }
  if (state != WebServerActivityState::SERVER_RUNNING) return;

  if (isApMode) fileTransferService().processDns();

  if (!isApMode && fileTransferService().webServerRunning() && !m4QemuNetWifiCompatConnected()) {
    static unsigned long lastWifiCheck = 0;
    if (millis() - lastWifiCheck > 2000) {
      lastWifiCheck = millis();
      if (!M4QemuNet::staConnected()) {
        showSetupError("WiFi connection lost");
        return;
      }
    }
  }
  if (!fileTransferService().webServerRunning()) return;

#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
  constexpr int kMaxIters = 12;
  constexpr unsigned long kBudgetMs = 10;
#else
  constexpr int kMaxIters = 40;
  constexpr unsigned long kBudgetMs = 20;
#endif
  const bool abortRequested = fileTransferService().handleWebClients(
      kMaxIters, kBudgetMs, &CrossPointWebServerActivity::webPumpAbortCheck, this);
  if (abortRequested) {
    logNavigationRequest(state, isApMode);
    requestNavigationExit();
    return;
  }
  lastHandleClientTime = millis();
}

void CrossPointWebServerActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void CrossPointWebServerActivity::render() const {
  renderer.clearScreen();
  const auto pageHeight = renderer.getScreenHeight();
  if (state == WebServerActivityState::SERVER_RUNNING) {
    renderServerRunning();
  } else if (state == WebServerActivityState::AP_STARTING) {
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, pageHeight / 2 - 20, "Starting Hotspot...", true,
                           EpdFontFamily::BOLD);
  } else if (state == WebServerActivityState::ERROR) {
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, pageHeight / 2 - 55, "Network setup failed", true,
                           EpdFontFamily::BOLD);
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight / 2 - 15, setupError.c_str());
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight / 2 + 25, "Back: choose another mode");
  } else {
    return;
  }
  renderer.displayBuffer();
}

void CrossPointWebServerActivity::renderServerRunning() const {
  constexpr int LINE_SPACING = 28;
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, 15, L(Str::kFileTransfer), true, EpdFontFamily::BOLD);

  if (isApMode) {
    int startY = 55;
    const std::string ssidInfo = std::string(L(Str::kWifiName)) + connectedSSID;
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, startY + LINE_SPACING, ssidInfo.c_str());
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 2, L(Str::kConnectPhoneToHotspot));
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 3, L(Str::kOrScanQRConnectHotspot));
    const auto pageWidth = renderer.getScreenWidth();
    const std::string wifiConfig = std::string("WIFI:S:") + connectedSSID + ";;";
    QRCodeHelper::drawQRCode(renderer, (pageWidth - QRCodeHelper::qrSize()) / 2, startY + LINE_SPACING * 5,
                             wifiConfig);
    startY += QRCodeHelper::qrSize() - 4 * QRCodeHelper::DEFAULT_PX + 4 * LINE_SPACING;
    const std::string hostnameUrl = std::string("http://") + AP_HOSTNAME + ".local/";
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 4, L(Str::kOpenInPhoneBrowser));
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 5, connectedIP.c_str());
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 6, L(Str::kOrScanQRFileManager));
    QRCodeHelper::drawQRCode(renderer, (pageWidth - QRCodeHelper::qrSize()) / 2, startY + LINE_SPACING * 8,
                             hostnameUrl);
  } else {
    const int startY = 65;
    std::string ssidInfo = "Network: " + connectedSSID;
    if (ssidInfo.length() > 28) ssidInfo.replace(25, ssidInfo.length() - 25, "...");
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, startY, ssidInfo.c_str());
    const std::string ipInfo = "IP Address: " + connectedIP;
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, startY + LINE_SPACING, ipInfo.c_str());
    const std::string webInfo = "http://" + connectedIP + "/";
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, startY + LINE_SPACING * 2, webInfo.c_str(), true,
                           EpdFontFamily::BOLD);
    const std::string hostnameUrl = std::string("or http://") + AP_HOSTNAME + ".local/";
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 3, hostnameUrl.c_str());
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 4, "Open this URL in your browser");
    const auto pageWidth = renderer.getScreenWidth();
    QRCodeHelper::drawQRCode(renderer, (pageWidth - QRCodeHelper::qrSize()) / 2, startY + LINE_SPACING * 6,
                             webInfo);
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 5, "or scan QR code with your phone:");
  }

  const auto labels = mappedInput.mapLabels(L(Str::kBack), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
}