#include "CalibreConnectActivity.h"

#include <ESPmDNS.h>
#include <GfxRenderer.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>

#include "MappedInputManager.h"
#include "I18n.h"
#include "WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"

namespace {
constexpr const char* HOSTNAME = "crosspoint";

void logInternalHeap(const char* where) {
  Serial.printf("[%lu] [CAL] [MEM] %s internal_free=%u largest=%u\n", millis(), where,
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
}
}  // namespace

void CalibreConnectActivity::taskTrampoline(void* param) {
  static_cast<CalibreConnectActivity*>(param)->displayTaskLoop();
}

bool CalibreConnectActivity::ensureDisplayTask() {
  if (displayTaskHandle) return true;
  if (!renderingMutex) {
    renderingMutex = xSemaphoreCreateMutex();
    if (!renderingMutex) {
      Serial.printf("[%lu] [CAL] display mutex allocation failed; transfer remains active\n", millis());
      return false;
    }
  }
  const BaseType_t ok = xTaskCreate(&CalibreConnectActivity::taskTrampoline, "CalibreConnectTask", 2048,
                                    this, 1, &displayTaskHandle);
  if (ok != pdPASS) {
    displayTaskHandle = nullptr;
    vSemaphoreDelete(renderingMutex);
    renderingMutex = nullptr;
    Serial.printf("[%lu] [CAL] display task allocation failed; transfer remains active\n", millis());
    return false;
  }
  return true;
}

void CalibreConnectActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  // Do not allocate the 2KB display task while WifiSelection owns the screen.
  // WiFi/LWIP needs internal RAM more than a hidden parent renderer does.
  renderingMutex = nullptr;
  displayTaskHandle = nullptr;
  updateRequired = false;
  pendingStartServer = false;
  state = CalibreConnectState::WIFI_SELECTION;
  connectedIP.clear();
  connectedSSID.clear();
  lastHandleClientTime = 0;
  lastProgressReceived = 0;
  lastProgressTotal = 0;
  currentUploadName.clear();
  lastCompleteName.clear();
  lastCompleteAt = 0;
  exitRequested = false;

  if (WiFi.status() != WL_CONNECTED) {
    enterNewActivity(new WifiSelectionActivity(renderer, mappedInput,
                                               [this](const bool connected) { onWifiSelectionComplete(connected); }));
  } else {
    connectedIP = WiFi.localIP().toString().c_str();
    connectedSSID = WiFi.SSID().c_str();
    startWebServer();
  }
}

void CalibreConnectActivity::onExit() {
  ActivityWithSubactivity::onExit();
  pendingStartServer = false;
  stopWebServer();
  MDNS.end();

  delay(50);
  WiFi.disconnect(false);
  delay(30);
  WiFi.mode(WIFI_OFF);
  delay(30);

  if (displayTaskHandle && renderingMutex) xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  if (renderingMutex) {
    vSemaphoreDelete(renderingMutex);
    renderingMutex = nullptr;
  }
}

void CalibreConnectActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    exitActivity();
    onComplete();
    return;
  }

  // Capture child data before requesting its deferred exit. Do not allocate
  // Calibre HTTP/display resources until the child (and its display task) is
  // actually destroyed by pumpSubActivityFrame().
  if (subActivity) {
    connectedIP = static_cast<WifiSelectionActivity*>(subActivity.get())->getConnectedIP();
  } else {
    connectedIP = WiFi.localIP().toString().c_str();
  }
  connectedSSID = WiFi.SSID().c_str();
  exitActivity();
  pendingStartServer = true;
}

void CalibreConnectActivity::startWebServer() {
  pendingStartServer = false;
  state = CalibreConnectState::SERVER_STARTING;
  logInternalHeap("before server start (WiFi child released)");

  if (MDNS.begin(HOSTNAME)) {
    Serial.printf("[%lu] [CAL] mDNS started: http://%s.local/\n", millis(), HOSTNAME);
  }

  webServer.reset(new (std::nothrow) CrossPointWebServer());
  if (!webServer) {
    state = CalibreConnectState::ERROR;
    render();
    logInternalHeap("web server allocation failed");
    return;
  }
  webServer->begin();

  if (webServer->isRunning()) {
    state = CalibreConnectState::SERVER_RUNNING;
    // Render the first usable screen synchronously. Only then spend internal
    // RAM on the optional background status-refresh task.
    render();
    (void)ensureDisplayTask();
    updateRequired = false;
    logInternalHeap("after server start");
  } else {
    webServer.reset();
    state = CalibreConnectState::ERROR;
    render();
    logInternalHeap("server begin failed");
  }
}

void CalibreConnectActivity::stopWebServer() {
  if (webServer) {
    webServer->stop();
    webServer.reset();
  }
}

void CalibreConnectActivity::loop() {
  if (subActivity) {
    pumpSubActivityFrame();
    if (!subActivity && pendingStartServer) startWebServer();
    return;
  }

  if (pendingStartServer) startWebServer();

  auto touchRequestsBack = [this]() {
    if (!mappedInput.hasTouch()) return false;
    if (mappedInput.wasBackGesture()) return true;
    if (mappedInput.wasSwipe() == MappedInputManager::SwipeDir::Left) return true;
    int tx = 0, ty = 0;
    const int bottom = renderer.getScreenHeight() - 92;
    if (mappedInput.wasScreenTapped(tx, ty) && ty >= bottom) return true;
    if (mappedInput.wasScreenTouchDown(tx, ty) && ty >= bottom) return true;
    return false;
  };

  if (mappedInput.wasPressed(MappedInputManager::Button::Back) || touchRequestsBack()) {
    exitRequested = true;
  }

  if (webServer && webServer->isRunning()) {
    esp_task_wdt_reset();
    constexpr int MAX_ITERATIONS = 80;
    for (int i = 0; i < MAX_ITERATIONS && webServer->isRunning(); ++i) {
      webServer->handleClient();
      if ((i & 0x07) == 0x07) esp_task_wdt_reset();
      if ((i & 0x0F) == 0x0F) {
        yield();
        if (mappedInput.wasPressed(MappedInputManager::Button::Back) || touchRequestsBack()) {
          exitRequested = true;
          break;
        }
      }
    }
    lastHandleClientTime = millis();

    const auto status = webServer->getWsUploadStatus();
    bool changed = false;
    if (status.inProgress) {
      if (status.received != lastProgressReceived || status.total != lastProgressTotal ||
          status.filename != currentUploadName) {
        lastProgressReceived = status.received;
        lastProgressTotal = status.total;
        currentUploadName = status.filename;
        changed = true;
      }
    } else if (lastProgressReceived != 0 || lastProgressTotal != 0) {
      lastProgressReceived = 0;
      lastProgressTotal = 0;
      currentUploadName.clear();
      changed = true;
    }
    if (status.lastCompleteAt != 0 && status.lastCompleteAt != lastCompleteAt) {
      lastCompleteAt = status.lastCompleteAt;
      lastCompleteName = status.lastCompleteName;
      changed = true;
    }
    if (lastCompleteAt > 0 && (millis() - lastCompleteAt) >= 6000) {
      lastCompleteAt = 0;
      lastCompleteName.clear();
      changed = true;
    }
    if (changed) {
      if (displayTaskHandle) updateRequired = true;
      else render();
    }
  }

  if (exitRequested) {
    onComplete();
    return;
  }
}

void CalibreConnectActivity::displayTaskLoop() {
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

void CalibreConnectActivity::render() const {
  renderer.clearScreen();
  const auto pageHeight = renderer.getScreenHeight();
  if (state == CalibreConnectState::SERVER_RUNNING) {
    renderServerRunning();
  } else if (state == CalibreConnectState::SERVER_STARTING) {
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, pageHeight / 2 - 20, "Starting Calibre...", true,
                           EpdFontFamily::BOLD);
  } else if (state == CalibreConnectState::ERROR) {
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, pageHeight / 2 - 20, "Calibre setup failed", true,
                           EpdFontFamily::BOLD);
  } else {
    return;  // WifiSelection child owns the display.
  }
  renderer.displayBuffer();
}

void CalibreConnectActivity::renderServerRunning() const {
  constexpr int LINE_SPACING = 24;
  constexpr int SMALL_SPACING = 20;
  constexpr int SECTION_SPACING = 40;
  constexpr int TOP_PADDING = 14;
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, 15, "Connect to Calibre", true, EpdFontFamily::BOLD);

  int y = 55 + TOP_PADDING;
  M4UiText::drawCentered(renderer, UI_10_FONT_ID, y, "Network", true, EpdFontFamily::BOLD);
  y += LINE_SPACING;
  std::string ssidInfo = "Network: " + connectedSSID;
  if (ssidInfo.length() > 28) ssidInfo.replace(25, ssidInfo.length() - 25, "...");
  M4UiText::drawCentered(renderer, UI_10_FONT_ID, y, ssidInfo.c_str());
  M4UiText::drawCentered(renderer, UI_10_FONT_ID, y + LINE_SPACING, ("IP: " + connectedIP).c_str());

  y += LINE_SPACING * 2 + SECTION_SPACING;
  M4UiText::drawCentered(renderer, UI_10_FONT_ID, y, "Setup", true, EpdFontFamily::BOLD);
  y += LINE_SPACING;
  renderer.drawCenteredText(SMALL_FONT_ID, y, "1) Install CrossPoint Reader plugin");
  renderer.drawCenteredText(SMALL_FONT_ID, y + SMALL_SPACING, "2) Be on the same WiFi network");
  renderer.drawCenteredText(SMALL_FONT_ID, y + SMALL_SPACING * 2, "3) In Calibre: \"Send to device\"");
  renderer.drawCenteredText(SMALL_FONT_ID, y + SMALL_SPACING * 3, "Keep this screen open while sending");

  y += SMALL_SPACING * 3 + SECTION_SPACING;
  M4UiText::drawCentered(renderer, UI_10_FONT_ID, y, "Status", true, EpdFontFamily::BOLD);
  y += LINE_SPACING;
  if (lastProgressTotal > 0 && lastProgressReceived <= lastProgressTotal) {
    std::string label = "Receiving";
    if (!currentUploadName.empty()) {
      label += ": " + currentUploadName;
      if (label.length() > 34) label.replace(31, label.length() - 31, "...");
    }
    renderer.drawCenteredText(SMALL_FONT_ID, y, label.c_str());
    constexpr int barWidth = 300;
    constexpr int barHeight = 16;
    constexpr int barX = (480 - barWidth) / 2;
    GUI.drawProgressBar(renderer, Rect{barX, y + 22, barWidth, barHeight}, lastProgressReceived, lastProgressTotal);
    y += 40;
  }

  if (lastCompleteAt > 0 && (millis() - lastCompleteAt) < 6000) {
    std::string msg = "Received: " + lastCompleteName;
    if (msg.length() > 36) msg.replace(33, msg.length() - 33, "...");
    renderer.drawCenteredText(SMALL_FONT_ID, y, msg.c_str());
  }

  const auto labels = mappedInput.mapLabels(L(Str::kBack), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
