#include "CrossPointWebServerActivity.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <GfxRenderer.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

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
#include "util/QRCodeHelper.h"


using namespace NetworkConstants;

namespace {
// DNS server for captive portal (redirects all DNS queries to our IP)
DNSServer* dnsServer = nullptr;
}  // namespace

void CrossPointWebServerActivity::taskTrampoline(void* param) {
  auto* self = static_cast<CrossPointWebServerActivity*>(param);
  self->displayTaskLoop();
}

void CrossPointWebServerActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  Serial.printf("[%lu] [WEBACT] [MEM] Free heap at onEnter: %d bytes\n", millis(), ESP.getFreeHeap());

  renderingMutex = xSemaphoreCreateMutex();

  // Reset state
  state = WebServerActivityState::MODE_SELECTION;
  networkMode = NetworkMode::JOIN_NETWORK;
  isApMode = false;
  connectedIP.clear();
  connectedSSID.clear();
  lastHandleClientTime = 0;
  updateRequired = true;

  xTaskCreate(&CrossPointWebServerActivity::taskTrampoline, "WebServerActivityTask",
              2048,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );

  // USB / m4adb wifi_transfer: skip the 三选一 and open the transfer page on
  // an already-prepared STA link (real Wi-Fi or QEMU open_eth).
  //
  // Home "网络管理" uses autoStart=false and MUST show NetworkModeSelection
  // (hotspot / join Wi-Fi / Calibre). QEMU handles JOIN_NETWORK later via
  // open_eth instead of a dead Wi-Fi scan.
  if (autoStartSavedSta && M4QemuNet::staConnected()) {
    isApMode = false;
    connectedIP = M4QemuNet::localIpStd();
    connectedSSID = M4QemuNet::ssidStd();
    if (connectedIP.empty() && m4QemuNetWifiCompatConnected()) connectedIP = "10.0.2.15";
    if (connectedSSID.empty() && m4QemuNetWifiCompatConnected()) connectedSSID = "qemu-openeth";
    if (MDNS.begin(AP_HOSTNAME)) {
      Serial.printf("[%lu] [WEBACT] mDNS started: http://%s.local/\n", millis(), AP_HOSTNAME);
    }
    Serial.printf("[%lu] [WEBACT] autoStart STA ip=%s ssid=%s qemu_eth=%d\n", millis(), connectedIP.c_str(),
                  connectedSSID.c_str(), m4QemuNetIsUp() ? 1 : 0);
    startWebServer();
    return;
  }

  // Normal entry: three-way mode picker (产品路径)
  Serial.printf("[%lu] [WEBACT] Launching NetworkModeSelectionActivity...\n", millis());
  enterNewActivity(new NetworkModeSelectionActivity(
      renderer, mappedInput, [this](const NetworkMode mode) { onNetworkModeSelected(mode); },
      [this]() { onGoBack(); }  // Cancel goes back to home
      ));
}

void CrossPointWebServerActivity::onExit() {
  ActivityWithSubactivity::onExit();

  Serial.printf("[%lu] [WEBACT] [MEM] Free heap at onExit start: %d bytes\n", millis(), ESP.getFreeHeap());

  state = WebServerActivityState::SHUTTING_DOWN;

  // Stop the web server first (before disconnecting WiFi)
  stopWebServer();

  // Stop mDNS
  MDNS.end();

  // Stop DNS server if running (AP mode)
  if (dnsServer) {
    Serial.printf("[%lu] [WEBACT] Stopping DNS server...\n", millis());
    dnsServer->stop();
    delete dnsServer;
    dnsServer = nullptr;
  }

  // Brief wait for LWIP stack to flush pending packets
  delay(200);  // 增加延迟，确保网络栈充分清理

#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
  // QEMU: do not tear down the open_eth netif — plugins and m4adb still need it.
  // Only AP soft-AP (if ever started) would be cleaned; STA path is fake eth.
  if (isApMode) {
    Serial.printf("[%lu] [WEBACT] QEMU: stop softAP only (keep open_eth)\n", millis());
    WiFi.softAPdisconnect(true);
  } else {
    Serial.printf("[%lu] [WEBACT] QEMU: skip WiFi.disconnect/WIFI_OFF (open_eth stays up)\n", millis());
  }
#else
  // Disconnect WiFi gracefully
  if (isApMode) {
    Serial.printf("[%lu] [WEBACT] Stopping WiFi AP...\n", millis());
    WiFi.softAPdisconnect(true);
  } else {
    Serial.printf("[%lu] [WEBACT] Disconnecting WiFi (graceful)...\n", millis());
    WiFi.disconnect(false);  // false = don't erase credentials, send disconnect frame
  }
  delay(300);  // Allow disconnect frame to be sent

  Serial.printf("[%lu] [WEBACT] Setting WiFi mode OFF...\n", millis());
  WiFi.mode(WIFI_OFF);
  delay(300);  // Allow WiFi hardware to power down
#endif

  Serial.printf("[%lu] [WEBACT] [MEM] Free heap after WiFi disconnect: %d bytes\n", millis(), ESP.getFreeHeap());

  Serial.printf("[%lu] [WEBACT] Acquiring rendering mutex before task deletion...\n", millis());
  xSemaphoreTake(renderingMutex, portMAX_DELAY);

  Serial.printf("[%lu] [WEBACT] Deleting display task...\n", millis());
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
    Serial.printf("[%lu] [WEBACT] Display task deleted\n", millis());
  }

  Serial.printf("[%lu] [WEBACT] Deleting mutex...\n", millis());
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
  Serial.printf("[%lu] [WEBACT] Mutex deleted\n", millis());

  Serial.printf("[%lu] [WEBACT] [MEM] Free heap at onExit end: %d bytes\n", millis(), ESP.getFreeHeap());
}

void CrossPointWebServerActivity::onNetworkModeSelected(const NetworkMode mode) {
  const char* modeName = "Join Network";
  if (mode == NetworkMode::CONNECT_CALIBRE) {
    modeName = "Connect to Calibre";
  } else if (mode == NetworkMode::CREATE_HOTSPOT) {
    modeName = "Create Hotspot";
  }
  Serial.printf("[%lu] [WEBACT] Network mode selected: %s\n", millis(), modeName);

  networkMode = mode;
  isApMode = (mode == NetworkMode::CREATE_HOTSPOT);

  exitActivity();

  if (mode == NetworkMode::CONNECT_CALIBRE) {
    exitActivity();
    enterNewActivity(new CalibreConnectActivity(renderer, mappedInput, [this] {
      exitActivity();
      state = WebServerActivityState::MODE_SELECTION;
      enterNewActivity(new NetworkModeSelectionActivity(
          renderer, mappedInput, [this](const NetworkMode nextMode) { onNetworkModeSelected(nextMode); },
          [this]() { onGoBack(); }));
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
      Serial.printf("[%lu] [WEBACT] QEMU JOIN_NETWORK → open_eth STA ip=%s\n", millis(),
                    connectedIP.c_str());
      startWebServer();
      return;
    }
#endif
    Serial.printf("[%lu] [WEBACT] Turning on WiFi (STA mode)...\n", millis());
    WiFi.mode(WIFI_STA);

    state = WebServerActivityState::WIFI_SELECTION;
    Serial.printf("[%lu] [WEBACT] Launching WifiSelectionActivity...\n", millis());
    enterNewActivity(new WifiSelectionActivity(renderer, mappedInput,
                                               [this](const bool connected) { onWifiSelectionComplete(connected); }));
  } else {
#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
    if (mode == NetworkMode::CREATE_HOTSPOT && m4QemuNetWifiCompatConnected()) {
      isApMode = false;
      connectedIP = M4QemuNet::localIpStd();
      connectedSSID = M4QemuNet::ssidStd();
      if (connectedIP.empty()) connectedIP = "10.0.2.15";
      if (connectedSSID.empty()) connectedSSID = "qemu-openeth";
      Serial.printf("[%lu] [WEBACT] QEMU CREATE_HOTSPOT → open_eth transfer ip=%s "
                    "(softAP N/A; use http://127.0.0.1:18080/)\n",
                    millis(), connectedIP.c_str());
      startWebServer();
      return;
    }
#endif
    state = WebServerActivityState::AP_STARTING;
    updateRequired = true;
    startAccessPoint();
  }
}

void CrossPointWebServerActivity::onWifiSelectionComplete(const bool connected) {
  Serial.printf("[%lu] [WEBACT] WifiSelectionActivity completed, connected=%d\n", millis(), connected);

  if (connected) {
    connectedIP = static_cast<WifiSelectionActivity*>(subActivity.get())->getConnectedIP();
    if (connectedIP.empty() || connectedIP == "0.0.0.0") {
      connectedIP = M4QemuNet::localIpStd();
    }
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
    state = WebServerActivityState::MODE_SELECTION;
    enterNewActivity(new NetworkModeSelectionActivity(
        renderer, mappedInput, [this](const NetworkMode mode) { onNetworkModeSelected(mode); },
        [this]() { onGoBack(); }));
  }
}

void CrossPointWebServerActivity::startAccessPoint() {
  Serial.printf("[%lu] [WEBACT] Starting Access Point mode...\n", millis());
  Serial.printf("[%lu] [WEBACT] [MEM] Free heap before AP start: %d bytes\n", millis(), ESP.getFreeHeap());

  WiFi.mode(WIFI_AP);
  delay(100);

  bool apStarted;
  if (AP_PASSWORD && strlen(AP_PASSWORD) >= 8) {
    apStarted = WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, false, AP_MAX_CONNECTIONS);
  } else {
    apStarted = WiFi.softAP(AP_SSID, nullptr, AP_CHANNEL, false, AP_MAX_CONNECTIONS);
  }

  if (!apStarted) {
    Serial.printf("[%lu] [WEBACT] ERROR: Failed to start Access Point!\n", millis());
    onGoBack();
    return;
  }

  delay(100);

  const IPAddress apIP = WiFi.softAPIP();
  char ipStr[16];
  snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", apIP[0], apIP[1], apIP[2], apIP[3]);
  connectedIP = ipStr;
  connectedSSID = AP_SSID;

  Serial.printf("[%lu] [WEBACT] Access Point started!\n", millis());
  Serial.printf("[%lu] [WEBACT] SSID: %s\n", millis(), AP_SSID);
  Serial.printf("[%lu] [WEBACT] IP: %s\n", millis(), connectedIP.c_str());

  if (MDNS.begin(AP_HOSTNAME)) {
    Serial.printf("[%lu] [WEBACT] mDNS started: http://%s.local/\n", millis(), AP_HOSTNAME);
  } else {
    Serial.printf("[%lu] [WEBACT] WARNING: mDNS failed to start\n", millis());
  }

  dnsServer = new DNSServer();
  dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer->start(NetworkConstants::DNS_PORT, "*", apIP);
  Serial.printf("[%lu] [WEBACT] DNS server started for captive portal\n", millis());

  Serial.printf("[%lu] [WEBACT] [MEM] Free heap after AP start: %d bytes\n", millis(), ESP.getFreeHeap());

  startWebServer();
}

void CrossPointWebServerActivity::startWebServer() {
  Serial.printf("[%lu] [WEBACT] Starting web server...\n", millis());

  webServer.reset(new CrossPointWebServer());
  webServer->begin();

  if (webServer->isRunning()) {
    state = WebServerActivityState::SERVER_RUNNING;
    Serial.printf("[%lu] [WEBACT] Web server started successfully\n", millis());

    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    render();
    xSemaphoreGive(renderingMutex);
    Serial.printf("[%lu] [WEBACT] Rendered File Transfer screen\n", millis());
  } else {
    Serial.printf("[%lu] [WEBACT] ERROR: Failed to start web server!\n", millis());
    webServer.reset();
    onGoBack();
  }
}

void CrossPointWebServerActivity::stopWebServer() {
  if (webServer && webServer->isRunning()) {
    Serial.printf("[%lu] [WEBACT] Stopping web server...\n", millis());
    webServer->stop();
    Serial.printf("[%lu] [WEBACT] Web server stopped\n", millis());
  }
  webServer.reset();
}

void CrossPointWebServerActivity::loop() {
  if (subActivity) {
    // All child callbacks can request teardown/replacement. Pump through the
    // base lifecycle gate so the current child's loop frame returns before its
    // object is destroyed. Direct subActivity->loop() bypasses that guarantee.
    pumpSubActivityFrame();
    return;
  }

  if (state == WebServerActivityState::SHUTTING_DOWN) {
    Serial.printf("[%lu] [WEBACT] SHUTTING_DOWN → onGoBack\n", millis());
    onGoBack();
    return;
  }

  auto wantsExit = [this]() -> bool {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) return true;
    if (!mappedInput.hasTouch()) return false;
    if (mappedInput.wasBackGesture()) return true;
    if (mappedInput.wasHomeGesture()) return true;
    if (mappedInput.wasSwipe() == MappedInputManager::SwipeDir::Left) return true;
    int tx = 0;
    int ty = 0;
    const int bottom = renderer.getScreenHeight() - 92;
    if (mappedInput.wasScreenTapped(tx, ty) && ty >= bottom) return true;
    if (mappedInput.wasScreenTouchDown(tx, ty) && ty >= bottom) return true;
    return false;
  };
  if (wantsExit()) {
    onGoBack();
    return;
  }

  if (state != WebServerActivityState::SERVER_RUNNING) {
    return;
  }

  if (isApMode && dnsServer) {
    dnsServer->processNextRequest();
  }

  if (!isApMode && webServer && webServer->isRunning() && !m4QemuNetWifiCompatConnected()) {
    static unsigned long lastWifiCheck = 0;
    if (millis() - lastWifiCheck > 2000) {
      lastWifiCheck = millis();
      if (!M4QemuNet::staConnected()) {
        Serial.printf("[%lu] [WEBACT] STA link lost (wifi_status=%d) → exit\n", millis(),
                      static_cast<int>(WiFi.status()));
        onGoBack();
        return;
      }
      const int rssi = WiFi.RSSI();
      if (rssi < -75) {
        Serial.printf("[%lu] [WEBACT] Warning: Weak WiFi signal: %d dBm\n", millis(), rssi);
      }
    }
  }

  if (!webServer || !webServer->isRunning()) {
    return;
  }

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
  if (state == WebServerActivityState::SERVER_RUNNING) {
    renderer.clearScreen();
    renderServerRunning();
    renderer.displayBuffer();
  } else if (state == WebServerActivityState::AP_STARTING) {
    renderer.clearScreen();
    const auto pageHeight = renderer.getScreenHeight();
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, pageHeight / 2 - 20, "Starting Hotspot...", true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
  }
}

void CrossPointWebServerActivity::renderServerRunning() const {
  constexpr int LINE_SPACING = 28;

  M4UiText::drawCentered(renderer, UI_12_FONT_ID, 15, L(Str::kFileTransfer), true, EpdFontFamily::BOLD);

  if (isApMode) {
    int startY = 55;

    std::string ssidInfo = std::string(L(Str::kWifiName)) + connectedSSID;
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, startY + LINE_SPACING, ssidInfo.c_str());

    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 2, L(Str::kConnectPhoneToHotspot));

    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 3,
                              L(Str::kOrScanQRConnectHotspot));
    const auto pageWidth = renderer.getScreenWidth();
    const std::string wifiConfig = std::string("WIFI:S:") + connectedSSID + ";;";
    QRCodeHelper::drawQRCode(renderer, (pageWidth - QRCodeHelper::qrSize()) / 2, startY + LINE_SPACING * 5, wifiConfig);

    startY += QRCodeHelper::qrSize() - 4 * QRCodeHelper::DEFAULT_PX + 4 * LINE_SPACING;

    std::string hostnameUrl = std::string("http://") + AP_HOSTNAME + ".local/";
    std::string ipUrl = connectedIP;
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 4, L(Str::kOpenInPhoneBrowser));
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 5, ipUrl.c_str());
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 6, L(Str::kOrScanQRFileManager));
    QRCodeHelper::drawQRCode(renderer, (pageWidth - QRCodeHelper::qrSize()) / 2, startY + LINE_SPACING * 8,
                             hostnameUrl);
  } else {
    const int startY = 65;

    std::string ssidInfo = "Network: " + connectedSSID;
    if (ssidInfo.length() > 28) {
      ssidInfo.replace(25, ssidInfo.length() - 25, "...");
    }
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, startY, ssidInfo.c_str());

    std::string ipInfo = "IP Address: " + connectedIP;
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, startY + LINE_SPACING, ipInfo.c_str());

    std::string webInfo = "http://" + connectedIP + "/";
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, startY + LINE_SPACING * 2, webInfo.c_str(), true, EpdFontFamily::BOLD);

    std::string hostnameUrl = std::string("or http://") + AP_HOSTNAME + ".local/";
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 3, hostnameUrl.c_str());

    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 4, "Open this URL in your browser");

    const auto pageWidth = renderer.getScreenWidth();
    QRCodeHelper::drawQRCode(renderer, (pageWidth - QRCodeHelper::qrSize()) / 2, startY + LINE_SPACING * 6, webInfo);
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 5, "or scan QR code with your phone:");
  }

  const auto labels = mappedInput.mapLabels(L(Str::kBack), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
}
