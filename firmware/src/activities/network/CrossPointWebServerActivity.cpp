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

  // USB wifi_transfer has already performed the bounded saved-credential
  // connection.  Jump straight to the native transfer screen instead of
  // forcing a second scan/menu interaction.  If the link disappeared between
  // the bridge reply and activity entry, retain the normal touch flow.
  if (autoStartSavedSta && WiFi.status() == WL_CONNECTED &&
      WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    isApMode = false;
    connectedIP = WiFi.localIP().toString().c_str();
    connectedSSID = WiFi.SSID().c_str();
    if (MDNS.begin(AP_HOSTNAME)) {
      Serial.printf("[%lu] [WEBACT] mDNS started: http://%s.local/\n", millis(), AP_HOSTNAME);
    }
    startWebServer();
    return;
  }

  // Launch network mode selection subactivity
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

  // 注意：Arduino WiFi 库已经封装了 esp_wifi_stop/deinit
  // WiFi.mode(WIFI_OFF) 内部会调用 esp_wifi_stop()
  // 但不会调用 esp_wifi_deinit()，因为 Arduino 假设 WiFi 可能会被重新使用
  // 在 ESP32-C3 上，WiFi 协议栈占用的内存约 30-50KB
  // 由于我们使用 Arduino 框架，无法强制释放这部分内存
  // 只能通过减少 WiFi 使用频率来缓解

  Serial.printf("[%lu] [WEBACT] [MEM] Free heap after WiFi disconnect: %d bytes\n", millis(), ESP.getFreeHeap());

  // Acquire mutex before deleting task
  Serial.printf("[%lu] [WEBACT] Acquiring rendering mutex before task deletion...\n", millis());
  xSemaphoreTake(renderingMutex, portMAX_DELAY);

  // Delete the display task
  Serial.printf("[%lu] [WEBACT] Deleting display task...\n", millis());
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
    Serial.printf("[%lu] [WEBACT] Display task deleted\n", millis());
  }

  // Delete the mutex
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

  // Exit mode selection subactivity
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
    // STA mode - launch WiFi selection
    Serial.printf("[%lu] [WEBACT] Turning on WiFi (STA mode)...\n", millis());
    WiFi.mode(WIFI_STA);

    state = WebServerActivityState::WIFI_SELECTION;
    Serial.printf("[%lu] [WEBACT] Launching WifiSelectionActivity...\n", millis());
    enterNewActivity(new WifiSelectionActivity(renderer, mappedInput,
                                               [this](const bool connected) { onWifiSelectionComplete(connected); }));
  } else {
    // AP mode - start access point
    state = WebServerActivityState::AP_STARTING;
    updateRequired = true;
    startAccessPoint();
  }
}

void CrossPointWebServerActivity::onWifiSelectionComplete(const bool connected) {
  Serial.printf("[%lu] [WEBACT] WifiSelectionActivity completed, connected=%d\n", millis(), connected);

  if (connected) {
    // Get connection info before exiting subactivity
    connectedIP = static_cast<WifiSelectionActivity*>(subActivity.get())->getConnectedIP();
    connectedSSID = WiFi.SSID().c_str();
    isApMode = false;

    exitActivity();

    // Start mDNS for hostname resolution
    if (MDNS.begin(AP_HOSTNAME)) {
      Serial.printf("[%lu] [WEBACT] mDNS started: http://%s.local/\n", millis(), AP_HOSTNAME);
    }

    // Start the web server
    startWebServer();
  } else {
    // User cancelled - go back to mode selection
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

  // Configure and start the AP
  WiFi.mode(WIFI_AP);
  delay(100);

  // Start soft AP
  bool apStarted;
  if (AP_PASSWORD && strlen(AP_PASSWORD) >= 8) {
    apStarted = WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, false, AP_MAX_CONNECTIONS);
  } else {
    // Open network (no password)
    apStarted = WiFi.softAP(AP_SSID, nullptr, AP_CHANNEL, false, AP_MAX_CONNECTIONS);
  }

  if (!apStarted) {
    Serial.printf("[%lu] [WEBACT] ERROR: Failed to start Access Point!\n", millis());
    onGoBack();
    return;
  }

  delay(100);  // Wait for AP to fully initialize

  // Get AP IP address
  const IPAddress apIP = WiFi.softAPIP();
  char ipStr[16];
  snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", apIP[0], apIP[1], apIP[2], apIP[3]);
  connectedIP = ipStr;
  connectedSSID = AP_SSID;

  Serial.printf("[%lu] [WEBACT] Access Point started!\n", millis());
  Serial.printf("[%lu] [WEBACT] SSID: %s\n", millis(), AP_SSID);
  Serial.printf("[%lu] [WEBACT] IP: %s\n", millis(), connectedIP.c_str());

  // Start mDNS for hostname resolution
  if (MDNS.begin(AP_HOSTNAME)) {
    Serial.printf("[%lu] [WEBACT] mDNS started: http://%s.local/\n", millis(), AP_HOSTNAME);
  } else {
    Serial.printf("[%lu] [WEBACT] WARNING: mDNS failed to start\n", millis());
  }

  // Start DNS server for captive portal behavior
  // This redirects all DNS queries to our IP, making any domain typed resolve to us
  dnsServer = new DNSServer();
  dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer->start(NetworkConstants::DNS_PORT, "*", apIP);
  Serial.printf("[%lu] [WEBACT] DNS server started for captive portal\n", millis());

  Serial.printf("[%lu] [WEBACT] [MEM] Free heap after AP start: %d bytes\n", millis(), ESP.getFreeHeap());

  // Start the web server
  startWebServer();
}

void CrossPointWebServerActivity::startWebServer() {
  Serial.printf("[%lu] [WEBACT] Starting web server...\n", millis());

  // Create the web server instance
  webServer.reset(new CrossPointWebServer());
  webServer->begin();

  if (webServer->isRunning()) {
    state = WebServerActivityState::SERVER_RUNNING;
    Serial.printf("[%lu] [WEBACT] Web server started successfully\n", millis());

    // Force an immediate render since we're transitioning from a subactivity
    // that had its own rendering task. We need to make sure our display is shown.
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    render();
    xSemaphoreGive(renderingMutex);
    Serial.printf("[%lu] [WEBACT] Rendered File Transfer screen\n", millis());
  } else {
    Serial.printf("[%lu] [WEBACT] ERROR: Failed to start web server!\n", millis());
    webServer.reset();
    // Go back on error
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
    // Forward loop to subactivity
    subActivity->loop();
    return;
  }

  // The transfer page used to expose only the legacy hardware button hints.
  // On M4 the display is touch-first, so a user could enter this page and be
  // unable to leave: no touch hit-test ran while the HTTP server was active.
  // Keep the hit area aligned with the painted bottom hint and accept the
  // same edge/back gesture used by the other list activities.
  auto touchRequestsBack = [this]() {
    if (!mappedInput.hasTouch()) return false;
    if (mappedInput.wasBackGesture()) return true;
    if (mappedInput.wasSwipe() == MappedInputManager::SwipeDir::Left) return true;
    int tx = 0;
    int ty = 0;
    const int bottom = renderer.getScreenHeight() - 92;
    if (mappedInput.wasScreenTapped(tx, ty) && ty >= bottom) return true;
    if (mappedInput.wasScreenTouchDown(tx, ty) && ty >= bottom) return true;
    return false;
  };
  if (touchRequestsBack()) {
    onGoBack();
    return;
  }

  // Handle different states
  if (state == WebServerActivityState::SERVER_RUNNING) {
    // Handle DNS requests for captive portal (AP mode only)
    if (isApMode && dnsServer) {
      dnsServer->processNextRequest();
    }

    // STA mode: Monitor WiFi connection health
    if (!isApMode && webServer && webServer->isRunning()) {
      static unsigned long lastWifiCheck = 0;
      if (millis() - lastWifiCheck > 2000) {  // Check every 2 seconds
        lastWifiCheck = millis();
        const wl_status_t wifiStatus = WiFi.status();
        if (wifiStatus != WL_CONNECTED) {
          Serial.printf("[%lu] [WEBACT] WiFi disconnected! Status: %d\n", millis(), wifiStatus);
          // Show error and exit gracefully
          state = WebServerActivityState::SHUTTING_DOWN;
          updateRequired = true;
          return;
        }
        // Log weak signal warnings
        const int rssi = WiFi.RSSI();
        if (rssi < -75) {
          Serial.printf("[%lu] [WEBACT] Warning: Weak WiFi signal: %d dBm\n", millis(), rssi);
        }
      }
    }

    // Handle web server requests - maximize throughput with watchdog safety
    if (webServer && webServer->isRunning()) {
      const unsigned long timeSinceLastHandleClient = millis() - lastHandleClientTime;

      // Log if there's a significant gap between handleClient calls (>100ms)
      if (lastHandleClientTime > 0 && timeSinceLastHandleClient > 100) {
        Serial.printf("[%lu] [WEBACT] WARNING: %lu ms gap since last handleClient\n", millis(),
                      timeSinceLastHandleClient);
      }

      // Reset watchdog BEFORE processing - HTTP header parsing can be slow
      esp_task_wdt_reset();

      // Process HTTP requests in tight loop for maximum throughput
      // More iterations = more data processed per main loop cycle
      constexpr int MAX_ITERATIONS = 500;
      for (int i = 0; i < MAX_ITERATIONS && webServer->isRunning(); i++) {
        webServer->handleClient();
        // Reset watchdog every 32 iterations
        if ((i & 0x1F) == 0x1F) {
          esp_task_wdt_reset();
        }
        // Yield and check for exit button every 64 iterations
        if ((i & 0x3F) == 0x3F) {
          yield();
          // Force trigger an update of which buttons are being pressed so be have accurate state
          // for back button checking
          mappedInput.update();
          // Check for exit button inside loop for responsiveness
          if (mappedInput.wasPressed(MappedInputManager::Button::Back) || touchRequestsBack()) {
            onGoBack();
            return;
          }
        }
      }
      lastHandleClientTime = millis();
    }

    // Handle exit on Back button (also check outside loop)
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) || touchRequestsBack()) {
      onGoBack();
      return;
    }
  }
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
  // Only render our own UI when server is running
  // Subactivities handle their own rendering
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
  // Use consistent line spacing
  constexpr int LINE_SPACING = 28;  // Space between lines

  M4UiText::drawCentered(renderer, UI_12_FONT_ID, 15, L(Str::kFileTransfer), true, EpdFontFamily::BOLD);

  if (isApMode) {
    // AP mode display - center the content block
    int startY = 55;

    std::string ssidInfo = std::string(L(Str::kWifiName)) + connectedSSID;
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, startY + LINE_SPACING, ssidInfo.c_str());

    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 2, L(Str::kConnectPhoneToHotspot));

    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 3,
                              L(Str::kOrScanQRConnectHotspot));
    // Show QR code for WiFi
    const auto pageWidth = renderer.getScreenWidth();
    const std::string wifiConfig = std::string("WIFI:S:") + connectedSSID + ";;";
    
    QRCodeHelper::drawQRCode(renderer, (pageWidth - QRCodeHelper::qrSize()) / 2, startY + LINE_SPACING * 5, wifiConfig);

    startY += QRCodeHelper::qrSize() - 4 * QRCodeHelper::DEFAULT_PX + 4 * LINE_SPACING;

    std::string hostnameUrl = std::string("http://") + AP_HOSTNAME + ".local/";

    // Show IP address as fallback
    std::string ipUrl = connectedIP;
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 4, L(Str::kOpenInPhoneBrowser));
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 5, ipUrl.c_str());

    // Show QR code for URL
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 6, L(Str::kOrScanQRFileManager));
    
    QRCodeHelper::drawQRCode(renderer, (pageWidth - QRCodeHelper::qrSize()) / 2, startY + LINE_SPACING * 8,
                             hostnameUrl);
  } else {
    // STA mode display (original behavior)
    const int startY = 65;

    std::string ssidInfo = "Network: " + connectedSSID;
    if (ssidInfo.length() > 28) {
      ssidInfo.replace(25, ssidInfo.length() - 25, "...");
    }
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, startY, ssidInfo.c_str());

    std::string ipInfo = "IP Address: " + connectedIP;
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, startY + LINE_SPACING, ipInfo.c_str());

    // Show web server URL prominently
    std::string webInfo = "http://" + connectedIP + "/";
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, startY + LINE_SPACING * 2, webInfo.c_str(), true, EpdFontFamily::BOLD);

    // Also show hostname URL
    std::string hostnameUrl = std::string("or http://") + AP_HOSTNAME + ".local/";
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 3, hostnameUrl.c_str());

    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 4, "Open this URL in your browser");

    // Show QR code for URL
   
    const auto pageWidth = renderer.getScreenWidth();
    QRCodeHelper::drawQRCode(renderer, (pageWidth - QRCodeHelper::qrSize()) / 2, startY + LINE_SPACING * 6, webInfo);
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 5, "or scan QR code with your phone:");
  }

  const auto labels = mappedInput.mapLabels(L(Str::kBack), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
}
