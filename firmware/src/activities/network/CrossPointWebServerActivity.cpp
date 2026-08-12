#include "CrossPointWebServerActivity.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <GfxRenderer.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>

#include <cstddef>
#include <new>

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
#include "util/QRCodeHelper.h"

using namespace NetworkConstants;

namespace {
DNSServer* dnsServer = nullptr;
void logInternalHeap(const char* where) {
  Serial.printf("[%lu] [WEBACT] [MEM] %s internal_free=%u largest=%u total_heap=%u\n", millis(), where,
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                static_cast<unsigned>(ESP.getFreeHeap()));
}
}  // namespace

void CrossPointWebServerActivity::taskTrampoline(void* param) {
  static_cast<CrossPointWebServerActivity*>(param)->displayTaskLoop();
}

void CrossPointWebServerActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  logInternalHeap("onEnter");
  renderingMutex = xSemaphoreCreateMutex();
  state = WebServerActivityState::MODE_SELECTION;
  networkMode = NetworkMode::JOIN_NETWORK;
  isApMode = false;
  connectedIP.clear();
  connectedSSID.clear();
  setupError.clear();
  lastHandleClientTime = 0;
  updateRequired = true;

  xTaskCreate(&CrossPointWebServerActivity::taskTrampoline, "WebServerActivityTask", 2048, this, 1,
              &displayTaskHandle);

  if (autoStartSavedSta && M4QemuNet::staConnected()) {
    isApMode = false;
    connectedIP = M4QemuNet::localIpStd();
    connectedSSID = M4QemuNet::ssidStd();
    if (connectedIP.empty() && m4QemuNetWifiCompatConnected()) connectedIP = "10.0.2.15";
    if (connectedSSID.empty() && m4QemuNetWifiCompatConnected()) connectedSSID = "qemu-openeth";
    if (MDNS.begin(AP_HOSTNAME)) {
      Serial.printf("[%lu] [WEBACT] mDNS started: http://%s.local/\n", millis(), AP_HOSTNAME);
    }
    startWebServer();
    return;
  }
  reopenModeSelection();
}

void CrossPointWebServerActivity::onExit() {
  ActivityWithSubactivity::onExit();
  state = WebServerActivityState::SHUTTING_DOWN;
  stopWebServer();
  MDNS.end();
  if (dnsServer) {
    dnsServer->stop();
    delete dnsServer;
    dnsServer = nullptr;
  }
  delay(200);
#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
  if (isApMode) WiFi.softAPdisconnect(true);
#else
  if (isApMode) WiFi.softAPdisconnect(true);
  else WiFi.disconnect(false);
  delay(300);
  WiFi.mode(WIFI_OFF);
  delay(300);
#endif
  logInternalHeap("after network stop");
  if (renderingMutex) xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  if (renderingMutex) {
    vSemaphoreDelete(renderingMutex);
    renderingMutex = nullptr;
  }
}

void CrossPointWebServerActivity::showSetupError(const char* message) {
  // Error UI must not retain the resources that just failed. In particular an
  // AP can succeed while the HTTP server fails; keeping AP/DNS alive here
  // fragments internal heap and makes a retry less likely to succeed.
  stopWebServer();
  MDNS.end();
  if (dnsServer) {
    dnsServer->stop();
    delete dnsServer;
    dnsServer = nullptr;
  }
#if !defined(M4_QEMU_PLUGIN_DEBUG) || !M4_QEMU_PLUGIN_DEBUG
  if (isApMode) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
  }
#endif
  setupError = message ? message : "Network setup failed";
  state = WebServerActivityState::ERROR;
  updateRequired = true;
  logInternalHeap(setupError.c_str());
}

void CrossPointWebServerActivity::reopenModeSelection() {
  setupError.clear();
  state = WebServerActivityState::MODE_SELECTION;
  enterNewActivity(new NetworkModeSelectionActivity(
      renderer, mappedInput, [this](const NetworkMode mode) { onNetworkModeSelected(mode); },
      [this]() { onGoBack(); }));
}

void CrossPointWebServerActivity::onNetworkModeSelected(const NetworkMode mode) {
  networkMode = mode;
  isApMode = (mode == NetworkMode::CREATE_HOTSPOT);
  exitActivity();

  if (mode == NetworkMode::CONNECT_CALIBRE) {
    enterNewActivity(new CalibreConnectActivity(renderer, mappedInput, [this] {
      exitActivity();
      reopenModeSelection();
    }));
    return;
  }

  if (mode == NetworkMode::JOIN_NETWORK) {
#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
    if (m4QemuNetWifiCompatConnected()) {
      isApMode = false;
      connectedIP = M4QemuNet::localIpStd();
      connectedSSID = M4QemuNet::ssidStd();
      if (connectedIP.empty()) connectedIP = "10.0.2.15";
      if (connectedSSID.empty()) connectedSSID = "qemu-openeth";
      startWebServer();
      return;
    }
#endif
    WiFi.mode(WIFI_STA);
    state = WebServerActivityState::WIFI_SELECTION;
    enterNewActivity(new WifiSelectionActivity(renderer, mappedInput,
                                               [this](const bool connected) { onWifiSelectionComplete(connected); }));
    return;
  }

#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
  if (m4QemuNetWifiCompatConnected()) {
    isApMode = false;
    connectedIP = M4QemuNet::localIpStd();
    connectedSSID = M4QemuNet::ssidStd();
    if (connectedIP.empty()) connectedIP = "10.0.2.15";
    if (connectedSSID.empty()) connectedSSID = "qemu-openeth";
    startWebServer();
    return;
  }
#endif
  state = WebServerActivityState::AP_STARTING;
  updateRequired = true;
  startAccessPoint();
}

void CrossPointWebServerActivity::onWifiSelectionComplete(const bool connected) {
  if (connected) {
    if (subActivity) {
      connectedIP = static_cast<WifiSelectionActivity*>(subActivity.get())->getConnectedIP();
    }
    if (connectedIP.empty() || connectedIP == "0.0.0.0") connectedIP = M4QemuNet::localIpStd();
    connectedSSID = M4QemuNet::ssidStd();
    if (connectedSSID.empty()) connectedSSID = WiFi.SSID().c_str();
    isApMode = false;
    exitActivity();
    if (MDNS.begin(AP_HOSTNAME)) {
      Serial.printf("[%lu] [WEBACT] mDNS started: http://%s.local/\n", millis(), AP_HOSTNAME);
    }
    startWebServer();
  } else {
    exitActivity();
    reopenModeSelection();
  }
}

void CrossPointWebServerActivity::startAccessPoint() {
  logInternalHeap("before AP start");
  WiFi.mode(WIFI_AP);
  delay(100);
  const bool apStarted = (AP_PASSWORD && strlen(AP_PASSWORD) >= 8)
                             ? WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, false, AP_MAX_CONNECTIONS)
                             : WiFi.softAP(AP_SSID, nullptr, AP_CHANNEL, false, AP_MAX_CONNECTIONS);
  if (!apStarted) {
    showSetupError("Hotspot startup failed");
    return;
  }
  delay(100);
  const IPAddress apIP = WiFi.softAPIP();
  char ipStr[16];
  snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", apIP[0], apIP[1], apIP[2], apIP[3]);
  connectedIP = ipStr;
  connectedSSID = AP_SSID;
  if (MDNS.begin(AP_HOSTNAME)) {
    Serial.printf("[%lu] [WEBACT] mDNS started: http://%s.local/\n", millis(), AP_HOSTNAME);
  }

  dnsServer = new (std::nothrow) DNSServer();
  if (dnsServer) {
    dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer->start(NetworkConstants::DNS_PORT, "*", apIP);
  } else {
    Serial.printf("[%lu] [WEBACT] WARN: no memory for captive DNS; direct IP still works\n", millis());
  }
  logInternalHeap("after AP start");
  startWebServer();
}

void CrossPointWebServerActivity::startWebServer() {
  webServer.reset(new (std::nothrow) CrossPointWebServer());
  if (!webServer) {
    showSetupError("Web server memory allocation failed");
    return;
  }
  webServer->begin();
  if (!webServer->isRunning()) {
    webServer.reset();
    showSetupError("Web server startup failed");
    return;
  }
  setupError.clear();
  state = WebServerActivityState::SERVER_RUNNING;
  updateRequired = true;
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  render();
  xSemaphoreGive(renderingMutex);
}

void CrossPointWebServerActivity::stopWebServer() {
  if (webServer) {
    if (webServer->isRunning()) webServer->stop();
    webServer.reset();
  }
}

void CrossPointWebServerActivity::loop() {
  if (subActivity) {
    pumpSubActivityFrame();
    return;
  }
  if (state == WebServerActivityState::SHUTTING_DOWN) {
    onGoBack();
    return;
  }

  auto wantsExit = [this]() -> bool {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) return true;
    if (!mappedInput.hasTouch()) return false;
    if (mappedInput.wasBackGesture() || mappedInput.wasHomeGesture()) return true;
    if (mappedInput.wasSwipe() == MappedInputManager::SwipeDir::Left) return true;
    int tx = 0, ty = 0;
    const int bottom = renderer.getScreenHeight() - 92;
    if (mappedInput.wasScreenTapped(tx, ty) && ty >= bottom) return true;
    if (mappedInput.wasScreenTouchDown(tx, ty) && ty >= bottom) return true;
    return false;
  };

  if (wantsExit()) {
    if (state == WebServerActivityState::ERROR) reopenModeSelection();
    else onGoBack();
    return;
  }
  if (state != WebServerActivityState::SERVER_RUNNING) return;

  if (isApMode && dnsServer) dnsServer->processNextRequest();

  if (!isApMode && webServer && webServer->isRunning() && !m4QemuNetWifiCompatConnected()) {
    static unsigned long lastWifiCheck = 0;
    if (millis() - lastWifiCheck > 2000) {
      lastWifiCheck = millis();
      if (!M4QemuNet::staConnected()) {
        showSetupError("WiFi connection lost");
        return;
      }
    }
  }
  if (!webServer || !webServer->isRunning()) return;

#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
  constexpr int kMaxIters = 12;
  constexpr unsigned long kBudgetMs = 10;
#else
  constexpr int kMaxIters = 40;
  constexpr unsigned long kBudgetMs = 20;
#endif
  esp_task_wdt_reset();
  const unsigned long t0 = millis();
  for (int i = 0; i < kMaxIters && webServer->isRunning(); ++i) {
    webServer->handleClient();
    if (static_cast<unsigned long>(millis() - t0) >= kBudgetMs) break;
    if ((i & 0x3) == 0x3) {
      yield();
      esp_task_wdt_reset();
      mappedInput.update();
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        onGoBack();
        return;
      }
    }
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
