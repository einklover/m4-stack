#include "WifiSelectionActivity.h"

#include "util/M4ListTouchPolicy.h"
#include <GfxRenderer.h>
#include <WiFi.h>

#include <map>

#include "MappedInputManager.h"
#include "I18n.h"
#include "WifiCredentialStore.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"
#include "BluetoothHIDManager.h"
#include "qemu/M4QemuNet.h"

void WifiSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<WifiSelectionActivity*>(param);
  self->displayTaskLoop();
}

void WifiSelectionActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  // Load saved WiFi credentials - SD card operations need lock as we use SPI for both
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  WIFI_STORE.loadFromFile();
  xSemaphoreGive(renderingMutex);

  // Reset state
  selectedNetworkIndex = 0;
  networks.clear();
  state = WifiSelectionState::SCANNING;
  selectedSSID.clear();
  connectedIP.clear();
  connectionError.clear();
  enteredPassword.clear();
  usedSavedPassword = false;
  failureTracker.reset();

  // Cache MAC address for display
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char macStr[32];
  snprintf(macStr, sizeof(macStr), "MAC address: %02x-%02x-%02x-%02x-%02x-%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
           mac[5]);
  cachedMacAddress = std::string(macStr);

  // Trigger first update to show scanning message
  updateRequired = true;

  xTaskCreate(&WifiSelectionActivity::taskTrampoline, "WifiSelectionTask",
              4096,               // Stack size (larger for WiFi operations)
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );

  // Start WiFi scan
  startWifiScan();
}

void WifiSelectionActivity::onExit() {
  Activity::onExit();

  Serial.printf("[%lu] [WIFI] [MEM] Free heap at onExit start: %d bytes\n", millis(), ESP.getFreeHeap());

  // Stop any ongoing WiFi scan
  Serial.printf("[%lu] [WIFI] Deleting WiFi scan...\n", millis());
  WiFi.scanDelete();
  Serial.printf("[%lu] [WIFI] [MEM] Free heap after scanDelete: %d bytes\n", millis(), ESP.getFreeHeap());

  // Note: We do NOT disconnect WiFi here - the parent activity (CrossPointWebServerActivity)
  // manages WiFi connection state. We just clean up the scan and task.

  // Acquire mutex before deleting task to ensure task isn't using it
  // This prevents hangs/crashes if the task holds the mutex when deleted
  Serial.printf("[%lu] [WIFI] Acquiring rendering mutex before task deletion...\n", millis());
  xSemaphoreTake(renderingMutex, portMAX_DELAY);

  // Delete the display task (we now hold the mutex, so task is blocked if it needs it)
  Serial.printf("[%lu] [WIFI] Deleting display task...\n", millis());
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
    Serial.printf("[%lu] [WIFI] Display task deleted\n", millis());
  }

  // Now safe to delete the mutex since we own it
  Serial.printf("[%lu] [WIFI] Deleting mutex...\n", millis());
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
  Serial.printf("[%lu] [WIFI] Mutex deleted\n", millis());

  Serial.printf("[%lu] [WIFI] [MEM] Free heap at onExit end: %d bytes\n", millis(), ESP.getFreeHeap());
}

void WifiSelectionActivity::startWifiScan() {
  state = WifiSelectionState::SCANNING;
  networks.clear();
  updateRequired = true;

  // Set WiFi mode to station
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  // Start async scan
  WiFi.scanNetworks(true);  // true = async scan
}

void WifiSelectionActivity::processWifiScanResults() {
  const int16_t scanResult = WiFi.scanComplete();

  if (scanResult == WIFI_SCAN_RUNNING) {
    // Scan still in progress
    return;
  }

  if (scanResult == WIFI_SCAN_FAILED) {
    state = WifiSelectionState::NETWORK_LIST;
    updateRequired = true;
    return;
  }

  // Scan complete, process results
  // Use a map to deduplicate networks by SSID, keeping the strongest signal
  std::map<std::string, WifiNetworkInfo> uniqueNetworks;

  for (int i = 0; i < scanResult; i++) {
    std::string ssid = WiFi.SSID(i).c_str();
    const int32_t rssi = WiFi.RSSI(i);

    // Skip hidden networks (empty SSID)
    if (ssid.empty()) {
      continue;
    }

    // Check if we've already seen this SSID
    auto it = uniqueNetworks.find(ssid);
    if (it == uniqueNetworks.end() || rssi > it->second.rssi) {
      // New network or stronger signal than existing entry
      WifiNetworkInfo network;
      network.ssid = ssid;
      network.rssi = rssi;
      network.isEncrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
      network.hasSavedPassword = WIFI_STORE.hasSavedCredential(network.ssid);
      uniqueNetworks[ssid] = network;
    }
  }

  // Convert map to vector
  networks.clear();
  for (const auto& pair : uniqueNetworks) {
    // cppcheck-suppress useStlAlgorithm
    networks.push_back(pair.second);
  }

  // Sort by signal strength (strongest first)
  std::sort(networks.begin(), networks.end(),
            [](const WifiNetworkInfo& a, const WifiNetworkInfo& b) { return a.rssi > b.rssi; });

  // Show networks with PW first
  std::sort(networks.begin(), networks.end(), [](const WifiNetworkInfo& a, const WifiNetworkInfo& b) {
    return a.hasSavedPassword && !b.hasSavedPassword;
  });

  WiFi.scanDelete();
  state = WifiSelectionState::NETWORK_LIST;
  selectedNetworkIndex = 0;
  updateRequired = true;
}

void WifiSelectionActivity::selectNetwork(const int index) {
  if (index < 0 || index >= static_cast<int>(networks.size())) {
    return;
  }

  const auto& network = networks[index];
  selectedSSID = network.ssid;
  selectedRequiresPassword = network.isEncrypted;
  usedSavedPassword = false;
  enteredPassword.clear();

  // Check if we have saved credentials for this network
  const auto* savedCred = WIFI_STORE.findCredential(selectedSSID);
  if (savedCred && !savedCred->password.empty()) {
    // Use saved password - connect directly
    enteredPassword = savedCred->password;
    usedSavedPassword = true;
    Serial.printf("[%lu] [WiFi] Using saved password for %s, length: %zu\n", millis(), selectedSSID.c_str(),
                  enteredPassword.size());
    attemptConnection();
    return;
  }

  if (selectedRequiresPassword) {
    // Show password entry
    state = WifiSelectionState::PASSWORD_ENTRY;
    // Don't allow screen updates while changing activity
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    enterNewActivity(new KeyboardEntryActivity(
        renderer, mappedInput, "Enter WiFi Password",
        "",     // No initial text
        50,     // Y position
        64,     // Max password length
        false,  // Show password by default (hard keyboard to use)
        [this](const std::string& text) {
          enteredPassword = text;
          exitActivity();
        },
        [this] {
          state = WifiSelectionState::NETWORK_LIST;
          updateRequired = true;
          exitActivity();
        }));
    updateRequired = true;
    xSemaphoreGive(renderingMutex);
  } else {
    // Connect directly for open networks
    attemptConnection();
  }
}

void WifiSelectionActivity::attemptConnection() {
  state = WifiSelectionState::CONNECTING;
  connectionStartTime = millis();
  connectedIP.clear();
  connectionError.clear();
  failureTracker.reset();
  updateRequired = true;
    // CRITICAL: Disable Bluetooth when enabling WiFi
  // ESP32-C3 cannot have both WiFi and BLE enabled simultaneously
  try {
    auto& btMgr = BluetoothHIDManager::getInstance();
    if (btMgr.isEnabled()) {
      Serial.printf("[%lu] [WIFI] Disabling Bluetooth to enable WiFi (mutual exclusion)\n", millis());
      btMgr.disable();
    }
  } catch (...) {
    Serial.printf("[%lu] [WIFI] Could not access Bluetooth manager\n", millis());
  }

  WiFi.mode(WIFI_STA);

  if (selectedRequiresPassword && !enteredPassword.empty()) {
    WiFi.begin(selectedSSID.c_str(), enteredPassword.c_str());
  } else {
    WiFi.begin(selectedSSID.c_str());
  }
}

void WifiSelectionActivity::checkConnectionStatus() {
  if (state != WifiSelectionState::CONNECTING) {
    return;
  }

  const wl_status_t status = WiFi.status();

  if (M4QemuNet::staConnected()) {
    // Successfully connected
    IPAddress ip = WiFi.localIP();
    char ipStr[16];
    snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    connectedIP = ipStr;

    // Sync time via NTP – configTime() handles re-init automatically
    configTime(8 * 3600, 0, "pool.ntp.org", "time.cloudflare.com");
    Serial.printf("[%lu] [WIFI] NTP sync started (UTC+8)\n", millis());

    if (!usedSavedPassword && !enteredPassword.empty()) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      const bool saved = WIFI_STORE.addCredential(selectedSSID, enteredPassword);
      xSemaphoreGive(renderingMutex);
      if (!saved) {
        connectionError = "Connected, but could not save Wi-Fi";
        state = WifiSelectionState::CONNECTION_FAILED;
        updateRequired = true;
        return;
      }
    }
    Serial.printf("[%lu] [WIFI] Connected with durable credentials, completing immediately\n", millis());
    onComplete(true);
    return;
  }

  if (failureTracker.authenticationFailed()) {
    if (usedSavedPassword) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      const bool removed = WIFI_STORE.removeCredential(selectedSSID);
      xSemaphoreGive(renderingMutex);
      const auto network = find_if(networks.begin(), networks.end(),
                                   [this](const WifiNetworkInfo& net) { return net.ssid == selectedSSID; });
      if (removed && network != networks.end()) network->hasSavedPassword = false;
      connectionError = removed ? "Password rejected. Enter it again."
                                : "Password rejected. Re-enter it in Settings.";
    } else {
      connectionError = "Password rejected. Enter it again.";
    }
    state = WifiSelectionState::CONNECTION_FAILED;
    updateRequired = true;
    return;
  }

  if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL || status == WL_CONNECTION_LOST) {
    if (status == WL_NO_SSID_AVAIL) {
      connectionError = "Error: Network not found";
    } else {
      connectionError = "Error: Wi-Fi connection failed";
    }
    state = WifiSelectionState::CONNECTION_FAILED;
    updateRequired = true;
    return;
  }

  // Check for timeout
  if (millis() - connectionStartTime > CONNECTION_TIMEOUT_MS) {
    WiFi.disconnect(false);
    connectionError = "Error: Connection timeout";
    state = WifiSelectionState::CONNECTION_FAILED;
    updateRequired = true;
    return;
  }
}

void WifiSelectionActivity::loop() {
  if (subActivity) {
    pumpSubActivityFrame();
    return;
  }

  // Shared touch helpers (geometry matches render* methods). Physical buttons unchanged below.
  auto fillTouchEvent = [this](M4ListTouchPolicy::Event& e) {
    e = {};
    if (!mappedInput.hasTouch()) return;
    // Sample each edge independently (no else-if): same-frame down+tap possible
    // if hardware reports both; resolveList prefers swipe → tap → down once.
    e.backGesture = mappedInput.wasBackGesture();
    const auto sw = mappedInput.wasSwipe();
    if (sw == MappedInputManager::SwipeDir::Up)
      e.swipe = M4ListTouchPolicy::Swipe::Up;
    else if (sw == MappedInputManager::SwipeDir::Down)
      e.swipe = M4ListTouchPolicy::Swipe::Down;
    else if (sw == MappedInputManager::SwipeDir::Left)
      e.swipe = M4ListTouchPolicy::Swipe::Left;
    else if (sw == MappedInputManager::SwipeDir::Right)
      e.swipe = M4ListTouchPolicy::Swipe::Right;
    int dx = 0, dy = 0, tx = 0, ty = 0;
    const bool down = mappedInput.wasScreenTouchDown(dx, dy);
    const bool tap = mappedInput.wasScreenTapped(tx, ty);
    e = M4ListTouchPolicy::mergeFrame(e.backGesture, e.swipe, down, dx, dy, tap, tx, ty);
  };

  // Check scan progress — allow cancel via back / edge gesture
  if (state == WifiSelectionState::SCANNING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        (mappedInput.hasTouch() && mappedInput.wasBackGesture())) {
      onComplete(false);
      return;
    }
    processWifiScanResults();
    return;
  }

  // Check connection progress — allow cancel back to list
  if (state == WifiSelectionState::CONNECTING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        (mappedInput.hasTouch() && mappedInput.wasBackGesture())) {
      WiFi.disconnect(false);
      state = WifiSelectionState::NETWORK_LIST;
      updateRequired = true;
      return;
    }
    checkConnectionStatus();
    return;
  }

  if (state == WifiSelectionState::PASSWORD_ENTRY) {
    // Reach here once password entry finished in subactivity
    attemptConnection();
    return;
  }

  // Handle connected state (should not normally be reached - connection completes immediately)
  if (state == WifiSelectionState::CONNECTED) {
    onComplete(true);
    return;
  }

  // Handle connection failed state
  if (state == WifiSelectionState::CONNECTION_FAILED) {
    int tx = 0, ty = 0;
    const bool touchDismiss =
        mappedInput.hasTouch() && (mappedInput.wasBackGesture() || mappedInput.wasScreenTapped(tx, ty));
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm) || touchDismiss) {
      state = WifiSelectionState::NETWORK_LIST;
      updateRequired = true;
      return;
    }
  }

  // Handle network list state
  if (state == WifiSelectionState::NETWORK_LIST) {
    // Touch path first (swipe before activate) — layout matches renderNetworkList()
    if (mappedInput.hasTouch()) {
      constexpr int startY = 60;
      constexpr int lineHeight = 25;
      const int pageHeight = renderer.getScreenHeight();
      const int maxVisible = (pageHeight - startY - 40) / lineHeight;
      const int itemCount = static_cast<int>(networks.size());

      M4ListTouchPolicy::Event te;
      fillTouchEvent(te);
      M4ListTouchPolicy::ListLayout layout;
      layout.listTop = startY;
      layout.listHeight = maxVisible * lineHeight;
      layout.rowStep = lineHeight;
      layout.itemCount = itemCount;
      layout.selectedIndex = selectedNetworkIndex;
      layout.maxVisible = maxVisible > 0 ? maxVisible : 1;

      int hit = -1;
      const auto act = M4ListTouchPolicy::resolveList(te, layout, hit);
      if (act == M4ListTouchPolicy::Action::Back) {
        onComplete(false);
        return;
      }
      if (act == M4ListTouchPolicy::Action::PageDown || act == M4ListTouchPolicy::Action::PageUp) {
        if (itemCount > 0) {
          selectedNetworkIndex = M4ListTouchPolicy::applyPage(selectedNetworkIndex, itemCount, maxVisible,
                                                              act == M4ListTouchPolicy::Action::PageDown);
          updateRequired = true;
        }
        return;
      }
      if (act == M4ListTouchPolicy::Action::Select && hit >= 0) {
        if (selectedNetworkIndex != hit) {
          selectedNetworkIndex = hit;
          updateRequired = true;
        }
        return;
      }
      if (act == M4ListTouchPolicy::Action::Activate && hit >= 0) {
        selectedNetworkIndex = hit;
        selectNetwork(selectedNetworkIndex);
        return;
      }
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      onComplete(false);
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (!networks.empty()) {
        selectNetwork(selectedNetworkIndex);
      } else {
        startWifiScan();
      }
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (selectedNetworkIndex > 0) {
        selectedNetworkIndex--;
        updateRequired = true;
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
               mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (!networks.empty() && selectedNetworkIndex < static_cast<int>(networks.size()) - 1) {
        selectedNetworkIndex++;
        updateRequired = true;
      }
    }
  }
}

std::string WifiSelectionActivity::getSignalStrengthIndicator(const int32_t rssi) const {
  // Convert RSSI to signal bars representation
  if (rssi >= -50) {
    return "||||";  // Excellent
  }
  if (rssi >= -60) {
    return "||| ";  // Good
  }
  if (rssi >= -70) {
    return "||  ";  // Fair
  }
  if (rssi >= -80) {
    return "|   ";  // Weak
  }
  return "    ";  // Very weak
}

void WifiSelectionActivity::displayTaskLoop() {
  while (true) {
    // If a subactivity is active, yield CPU time but don't render
    if (subActivity) {
      vTaskDelay(10 / portTICK_PERIOD_MS);
      continue;
    }

    // Don't render if we're in PASSWORD_ENTRY state - we're just transitioning
    // from the keyboard subactivity back to the main activity
    if (state == WifiSelectionState::PASSWORD_ENTRY) {
      vTaskDelay(10 / portTICK_PERIOD_MS);
      continue;
    }

    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void WifiSelectionActivity::render() const {
  renderer.clearScreen();

  switch (state) {
    case WifiSelectionState::SCANNING:
      renderConnecting();  // Reuse connecting screen with different message
      break;
    case WifiSelectionState::NETWORK_LIST:
      renderNetworkList();
      break;
    case WifiSelectionState::CONNECTING:
      renderConnecting();
      break;
    case WifiSelectionState::CONNECTED:
      renderConnected();
      break;
    case WifiSelectionState::CONNECTION_FAILED:
      renderConnectionFailed();
      break;
  }

  renderer.displayBuffer();
}

void WifiSelectionActivity::renderNetworkList() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Draw header
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, 15, "WiFi Networks", true, EpdFontFamily::BOLD);

  if (networks.empty()) {
    // No networks found or scan failed
    const auto height = renderer.getLineHeight(UI_10_FONT_ID);
    const auto top = (pageHeight - height) / 2;
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, top, "No networks found");
    renderer.drawCenteredText(SMALL_FONT_ID, top + height + 10, "Press Connect to scan again");
  } else {
    // Calculate how many networks we can display
    constexpr int startY = 60;
    constexpr int lineHeight = 25;
    const int maxVisibleNetworks = (pageHeight - startY - 40) / lineHeight;

    // Calculate scroll offset to keep selected item visible
    int scrollOffset = 0;
    if (selectedNetworkIndex >= maxVisibleNetworks) {
      scrollOffset = selectedNetworkIndex - maxVisibleNetworks + 1;
    }

    // Draw networks
    int displayIndex = 0;
    for (size_t i = scrollOffset; i < networks.size() && displayIndex < maxVisibleNetworks; i++, displayIndex++) {
      const int networkY = startY + displayIndex * lineHeight;
      const auto& network = networks[i];

      // Draw selection indicator
      if (static_cast<int>(i) == selectedNetworkIndex) {
        M4UiText::draw(renderer, UI_10_FONT_ID, 5, networkY, ">");
      }

      // Draw network name (truncate if too long)
      std::string displayName = network.ssid;
      if (displayName.length() > 33) {
        displayName.replace(30, displayName.length() - 30, "...");
      }
      M4UiText::draw(renderer, UI_10_FONT_ID, 20, networkY, displayName.c_str());

      // Draw signal strength indicator
      std::string signalStr = getSignalStrengthIndicator(network.rssi);
      M4UiText::draw(renderer, UI_10_FONT_ID, pageWidth - 90, networkY, signalStr.c_str());

      // Draw saved indicator (checkmark) for networks with saved passwords
      if (network.hasSavedPassword) {
        M4UiText::draw(renderer, UI_10_FONT_ID, pageWidth - 50, networkY, "+");
      }

      // Draw lock icon for encrypted networks
      if (network.isEncrypted) {
        M4UiText::draw(renderer, UI_10_FONT_ID, pageWidth - 30, networkY, "*");
      }
    }

    // Draw scroll indicators if needed
    if (scrollOffset > 0) {
      renderer.drawText(SMALL_FONT_ID, pageWidth - 15, startY - 10, "^");
    }
    if (scrollOffset + maxVisibleNetworks < static_cast<int>(networks.size())) {
      renderer.drawText(SMALL_FONT_ID, pageWidth - 15, startY + maxVisibleNetworks * lineHeight, "v");
    }

    // Show network count
    char countStr[32];
    snprintf(countStr, sizeof(countStr), "%zu networks found", networks.size());
    renderer.drawText(SMALL_FONT_ID, 20, pageHeight - 90, countStr);
  }

  // Show MAC address above the network count and legend
  renderer.drawText(SMALL_FONT_ID, 20, pageHeight - 105, cachedMacAddress.c_str());

  // Draw help text
  renderer.drawText(SMALL_FONT_ID, 20, pageHeight - 75, "* = Encrypted | + = Saved");
  const auto labels = mappedInput.mapLabels(L(Str::kBack), L(Str::kConnect), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::renderConnecting() const {
  const auto pageHeight = renderer.getScreenHeight();
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  if (state == WifiSelectionState::SCANNING) {
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, top, "Scanning...");
  } else {
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, top - 40, "Connecting...", true, EpdFontFamily::BOLD);

    std::string ssidInfo = "to " + selectedSSID;
    if (ssidInfo.length() > 25) {
      ssidInfo.replace(22, ssidInfo.length() - 22, "...");
    }
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, top, ssidInfo.c_str());
  }
}

void WifiSelectionActivity::renderConnected() const {
  const auto pageHeight = renderer.getScreenHeight();
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height * 4) / 2;

  M4UiText::drawCentered(renderer, UI_12_FONT_ID, top - 30, "Connected!", true, EpdFontFamily::BOLD);

  std::string ssidInfo = "Network: " + selectedSSID;
  if (ssidInfo.length() > 28) {
    ssidInfo.replace(25, ssidInfo.length() - 25, "...");
  }
  M4UiText::drawCentered(renderer, UI_10_FONT_ID, top + 10, ssidInfo.c_str());

  const std::string ipInfo = "IP Address: " + connectedIP;
  M4UiText::drawCentered(renderer, UI_10_FONT_ID, top + 40, ipInfo.c_str());

  // Use centralized button hints
  const auto labels = mappedInput.mapLabels("", "Continue", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::renderConnectionFailed() const {
  const auto pageHeight = renderer.getScreenHeight();
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height * 2) / 2;

  M4UiText::drawCentered(renderer, UI_12_FONT_ID, top - 20, L(Str::kConnectFailed), true, EpdFontFamily::BOLD);
  M4UiText::drawCentered(renderer, UI_10_FONT_ID, top + 20, connectionError.c_str());

  // Use centralized button hints
  const auto labels = mappedInput.mapLabels(L(Str::kBack), L(Str::kContinue), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
