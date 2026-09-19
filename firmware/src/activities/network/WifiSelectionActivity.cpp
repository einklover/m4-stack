#include "WifiSelectionActivity.h"

#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <WiFi.h>

#include <algorithm>
#include <map>

#include "BluetoothHIDManager.h"
#include "CrossPointSettings.h"
#include "I18n.h"
#include "MappedInputManager.h"
#include "WifiCredentialStore.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/M4WifiTransferPolicy.h"
#include "qemu/M4QemuNet.h"
#include "util/M4UiText.h"
#include "util/M4WifiSavePrompt.h"
#include "util/TouchUiGeometry.h"

#define SETTINGS CrossPointSettings::getInstance()

void WifiSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<WifiSelectionActivity*>(param);
  self->displayTaskLoop();
}

void WifiSelectionActivity::onEnter() {
  Activity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  WIFI_STORE.loadFromFile();
  xSemaphoreGive(renderingMutex);

  selectedNetworkIndex = 0;
  // Publish the empty model only while holding the render mutex; the display
  // task may still be painting the previous scan result.
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  networks.clear();
  xSemaphoreGive(renderingMutex);
  state = WifiSelectionState::SCANNING;
  selectedSSID.clear();
  connectedIP.clear();
  connectionError.clear();
  enteredPassword.clear();
  usedSavedPassword = false;
  saveRejectedAtCap = false;
  occupancyDenied = false;
  failureTracker.reset();

  if (purpose == M4WifiSelectionPurpose::SystemNetworking) {
    occupancyDenied =
        m4WifiSettingsExclusiveWhileTransfer() == M4NetworkAcquireResult::DeniedHeldByOther;
  } else {
    m4WifiNeedNetwork(M4NetworkOwner::WifiSettings);
  }

  uint8_t mac[6];
  WiFi.macAddress(mac);
  char macStr[32];
  snprintf(macStr, sizeof(macStr), "MAC: %02x-%02x-%02x-%02x-%02x-%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  cachedMacAddress = std::string(macStr);

  updateRequired = true;
  xTaskCreate(&WifiSelectionActivity::taskTrampoline, "WifiSelectionTask", 4096, this, 1, &displayTaskHandle);
  if (occupancyDenied) {
    connectionError = m4WifiTransferHoldsCopy();
    state = WifiSelectionState::CONNECTION_FAILED;
    updateRequired = true;
    return;
  }
  startWifiScan();
}

void WifiSelectionActivity::onExit() {
  Activity::onExit();
  m4WifiReleaseNetwork(M4NetworkOwner::WifiSettings);
  m4WifiReleaseNetwork(M4NetworkOwner::Ntp);
  WiFi.scanDelete();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void WifiSelectionActivity::startWifiScan() {
  if (purpose == M4WifiSelectionPurpose::SystemNetworking) {
    if (m4WifiSettingsExclusiveWhileTransfer() == M4NetworkAcquireResult::DeniedHeldByOther) {
      occupancyDenied = true;
      connectionError = m4WifiTransferHoldsCopy();
      state = WifiSelectionState::CONNECTION_FAILED;
      updateRequired = true;
      return;
    }
    occupancyDenied = false;
  }

  state = WifiSelectionState::SCANNING;
  // The display task may still be painting the previous list: clear the
  // shared model only while holding the render mutex.
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  networks.clear();
  selectedNetworkIndex = 0;
  xSemaphoreGive(renderingMutex);
  updateRequired = true;

#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
  // QEMU has no radio: with the compat link up, any radio touch (mode /
  // disconnect / scan) wedges the guest bridge. Land in the same empty-list
  // state as a failed scan; the list loop stays alive and pixels are unchanged.
  if (m4QemuNetWifiCompatConnected()) {
    state = WifiSelectionState::NETWORK_LIST;
    updateRequired = true;
    return;
  }
#endif

  WiFi.mode(WIFI_STA);
  if (m4WifiScanShouldDisconnectExistingSta(M4QemuNet::staConnected())) {
    WiFi.disconnect();
    delay(100);
  }
  WiFi.scanNetworks(true);
}

void WifiSelectionActivity::processWifiScanResults() {
  const int16_t scanResult = WiFi.scanComplete();
  if (scanResult == WIFI_SCAN_RUNNING) return;

  if (scanResult == WIFI_SCAN_FAILED) {
    state = WifiSelectionState::NETWORK_LIST;
    updateRequired = true;
    return;
  }

  std::map<std::string, WifiNetworkInfo> uniqueNetworks;
  for (int i = 0; i < scanResult; ++i) {
    const std::string ssid = WiFi.SSID(i).c_str();
    const int32_t rssi = WiFi.RSSI(i);
    if (ssid.empty()) continue;

    auto it = uniqueNetworks.find(ssid);
    if (it == uniqueNetworks.end() || rssi > it->second.rssi) {
      WifiNetworkInfo network;
      network.ssid = ssid;
      network.rssi = rssi;
      network.isEncrypted = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
      network.hasSavedPassword = WIFI_STORE.hasSavedCredential(network.ssid);
      uniqueNetworks[ssid] = network;
    }
  }

  // Stage off-lock: the display task may be painting the previous list.
  std::vector<WifiNetworkInfo> staged;
  staged.reserve(uniqueNetworks.size());
  for (const auto& pair : uniqueNetworks) staged.push_back(pair.second);
  std::sort(staged.begin(), staged.end(),
            [](const WifiNetworkInfo& a, const WifiNetworkInfo& b) { return a.rssi > b.rssi; });
  std::stable_sort(staged.begin(), staged.end(), [](const WifiNetworkInfo& a, const WifiNetworkInfo& b) {
    return a.hasSavedPassword && !b.hasSavedPassword;
  });

  WiFi.scanDelete();
  const std::string connected = currentConnectedSsid();
  int connectedIndex = 0;
  for (int i = 0; i < static_cast<int>(staged.size()); ++i) {
    if (staged[static_cast<size_t>(i)].ssid == connected) {
      connectedIndex = i;
      break;
    }
  }
  // Atomic publish: readers only ever see the old or the new vector,
  // never a half-rebuilt one. Nothing else runs under this lock.
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  networks.swap(staged);
  selectedNetworkIndex = connectedIndex;
  xSemaphoreGive(renderingMutex);
  state = WifiSelectionState::NETWORK_LIST;
  updateRequired = true;
  maybeAutoConnectKnown();
}

void WifiSelectionActivity::maybeAutoConnectKnown() {
  const std::string connected = currentConnectedSsid();
  if (!connected.empty()) return;
  if (!m4WifiShouldAutoConnectKnown(SETTINGS.wifiAlwaysReselect)) return;
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  int autoIndex = -1;
  for (int i = 0; i < static_cast<int>(networks.size()); ++i) {
    if (networks[static_cast<size_t>(i)].hasSavedPassword) {
      autoIndex = i;
      break;
    }
  }
  xSemaphoreGive(renderingMutex);
  if (autoIndex >= 0) selectNetwork(autoIndex);
}

void WifiSelectionActivity::selectNetwork(const int index) {
  // Copy the row under the render mutex: the display task may be painting
  // this same vector. Never hold a reference across the unlock.
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  const int scanned = static_cast<int>(networks.size());
  const bool hidden = m4WifiListRowIsHidden(index, scanned);
  const bool inRange = index >= 0 && index < scanned;
  WifiNetworkInfo network;
  if (inRange && !hidden) network = networks[static_cast<size_t>(index)];
  xSemaphoreGive(renderingMutex);
  if (hidden) {
    openHiddenNetworkSsidEntry();
    return;
  }
  if (!inRange) return;

  const std::string connected = currentConnectedSsid();
  if (m4WifiSelectActionForSsid(network.ssid.c_str(), connected.c_str(), purpose) ==
      M4WifiSelectAction::StayOnListCheckmark) {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    selectedNetworkIndex = index;
    xSemaphoreGive(renderingMutex);
    updateRequired = true;
    return;
  }
  if (purpose == M4WifiSelectionPurpose::SessionJoin && !connected.empty() &&
      connected == network.ssid) {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    selectedNetworkIndex = index;
    xSemaphoreGive(renderingMutex);
    onComplete(true);
    return;
  }

  selectedSSID = network.ssid;
  selectedRequiresPassword = network.isEncrypted;
  usedSavedPassword = false;
  enteredPassword.clear();

  const auto* savedCred = WIFI_STORE.findCredential(selectedSSID);
  if (savedCred && !savedCred->password.empty()) {
    enteredPassword = savedCred->password;
    usedSavedPassword = true;
    attemptConnection();
    return;
  }

  if (selectedRequiresPassword) {
    state = WifiSelectionState::PASSWORD_ENTRY;
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    enterNewActivity(new KeyboardEntryActivity(
        renderer, mappedInput, std::string("Wi-Fi 密码: ") + selectedSSID,
        "",
        18,
        64,
        true,
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
    attemptConnection();
  }
}

void WifiSelectionActivity::openHiddenNetworkSsidEntry() {
  selectedSSID.clear();
  enteredPassword.clear();
  usedSavedPassword = false;
  selectedRequiresPassword = true;
  state = WifiSelectionState::HIDDEN_SSID_ENTRY;
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  enterNewActivity(new KeyboardEntryActivity(
      renderer, mappedInput, "隐藏网络名称", "", 18, 32, false,
      [this](const std::string& text) {
        selectedSSID = text;
        exitActivity();
      },
      [this] {
        selectedSSID.clear();
        state = WifiSelectionState::NETWORK_LIST;
        updateRequired = true;
        exitActivity();
      }));
  updateRequired = true;
  xSemaphoreGive(renderingMutex);
}

void WifiSelectionActivity::beginConnectionAfterBtCheck() {
  bool btOn = false;
  try {
    btOn = BluetoothHIDManager::getInstance().isEnabled();
  } catch (...) {
    Serial.printf("[%lu] [WIFI] Could not access Bluetooth manager\n", millis());
  }
  if (btOn) {
    state = WifiSelectionState::CONFIRM_BT_DISCONNECT;
    updateRequired = true;
    return;
  }
  attemptConnection();
}

void WifiSelectionActivity::attemptConnection() {
  if (state != WifiSelectionState::CONFIRM_BT_DISCONNECT) {
    bool btOn = false;
    try {
      btOn = BluetoothHIDManager::getInstance().isEnabled();
    } catch (...) {
      Serial.printf("[%lu] [WIFI] Could not access Bluetooth manager\n", millis());
    }
    if (btOn) {
      state = WifiSelectionState::CONFIRM_BT_DISCONNECT;
      updateRequired = true;
      return;
    }
  }

  state = WifiSelectionState::CONNECTING;
  connectionStartTime = millis();
  connectedIP.clear();
  connectionError.clear();
  failureTracker.reset();
  updateRequired = true;

  WiFi.mode(WIFI_STA);
  if (selectedRequiresPassword && !enteredPassword.empty()) {
    WiFi.begin(selectedSSID.c_str(), enteredPassword.c_str());
  } else {
    WiFi.begin(selectedSSID.c_str());
  }
}

void WifiSelectionActivity::checkConnectionStatus() {
  if (state != WifiSelectionState::CONNECTING) return;

  const wl_status_t status = WiFi.status();
  if (M4QemuNet::staConnected()) {
    const IPAddress ip = WiFi.localIP();
    char ipStr[16];
    snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    connectedIP = ipStr;

    m4WifiNeedNetwork(M4NetworkOwner::Ntp);
    configTime(8 * 3600, 0, "pool.ntp.org", "time.cloudflare.com");

    // New credentials never auto-save: an explicit Yes/No prompt below owns
    // persistence. Saved/open successes complete directly (no prompt).
    if (m4WifiSavePromptDecision(usedSavedPassword, enteredPassword) == M4WifiSavePrompt::Needed) {
      state = WifiSelectionState::CONNECTED;
      updateRequired = true;
      return;
    }
    onComplete(true);
    return;
  }

  if (failureTracker.authenticationFailed()) {
    if (usedSavedPassword) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      const bool removed = WIFI_STORE.removeCredential(selectedSSID);
      const auto network = std::find_if(networks.begin(), networks.end(),
                                        [this](const WifiNetworkInfo& net) { return net.ssid == selectedSSID; });
      if (removed && network != networks.end()) network->hasSavedPassword = false;
      xSemaphoreGive(renderingMutex);
      connectionError = removed ? "密码错误，请重新输入" : "密码错误，请在设置中重新输入";
    } else {
      connectionError = "密码错误，请重新输入";
    }
    state = WifiSelectionState::CONNECTION_FAILED;
    updateRequired = true;
    return;
  }

  if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL || status == WL_CONNECTION_LOST) {
    connectionError = status == WL_NO_SSID_AVAIL ? "未找到这个 Wi-Fi" : "Wi-Fi 连接失败";
    state = WifiSelectionState::CONNECTION_FAILED;
    updateRequired = true;
    return;
  }

  if (millis() - connectionStartTime > CONNECTION_TIMEOUT_MS) {
    WiFi.disconnect(false);
    connectionError = "连接超时，请重试";
    state = WifiSelectionState::CONNECTION_FAILED;
    updateRequired = true;
  }
}

void WifiSelectionActivity::loop() {
  if (subActivity) {
    // Child exit/replace consumes this frame: the pumped child owned the
    // frame's input, so any parent frame/input use below would run on the
    // stale pre-transition frame. Repaint from the settled state next frame.
    if (pumpSubActivityFrame()) updateRequired = true;
    return;
  }

  if (state == WifiSelectionState::SCANNING) {
    if (mappedInput.wasBackGesture()) {
      onComplete(false);
      return;
    }
    processWifiScanResults();
    return;
  }

  if (state == WifiSelectionState::CONNECTING) {
    if (mappedInput.wasBackGesture()) {
      WiFi.disconnect(false);
      state = WifiSelectionState::NETWORK_LIST;
      updateRequired = true;
      return;
    }
    checkConnectionStatus();
    return;
  }

  if (state == WifiSelectionState::HIDDEN_SSID_ENTRY) {
    if (selectedSSID.empty()) {
      state = WifiSelectionState::NETWORK_LIST;
      updateRequired = true;
      return;
    }
    state = WifiSelectionState::PASSWORD_ENTRY;
    selectedRequiresPassword = true;
    usedSavedPassword = false;
    enteredPassword.clear();
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    enterNewActivity(new KeyboardEntryActivity(
        renderer, mappedInput, std::string("Wi-Fi 密码: ") + selectedSSID, "", 18, 64, true,
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
    return;
  }

  if (state == WifiSelectionState::CONFIRM_BT_DISCONNECT) {
    if (mappedInput.wasBackGesture() || mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state = WifiSelectionState::NETWORK_LIST;
      updateRequired = true;
      return;
    }
    M4ConfirmButton button = M4ConfirmButton::None;
    bool footerPrimaryOrRow = false;
    if (mappedInput.wasPressed(MappedInputManager::Button::Power)) {
      button = M4ConfirmButton::Power;
    }
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
        mappedInput.wasScreenTapped(tx, ty)) {
      button = M4ConfirmButton::Confirm;
      footerPrimaryOrRow = true;
    }
    if (m4WifiBtConfirmAccepts(button, footerPrimaryOrRow)) {
      try {
        auto& btMgr = BluetoothHIDManager::getInstance();
        if (btMgr.isEnabled()) btMgr.disable();
      } catch (...) {
        Serial.printf("[%lu] [WIFI] Could not access Bluetooth manager\n", millis());
      }
      attemptConnection();
    }
    return;
  }

  if (state == WifiSelectionState::PASSWORD_ENTRY) {
    beginConnectionAfterBtCheck();
    return;
  }

  if (state == WifiSelectionState::CONNECTED) {
    // Save prompt for the newly connected credential. Yes persists and
    // completes; No completes with the live session and persists nothing.
    const M4WifiSavePrompt prompt = m4WifiSavePromptDecision(usedSavedPassword, enteredPassword);
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty)) {
      if (saveRejectedAtCap) {
        onComplete(true);
        return;
      }
      if (m4WifiSaveConfirmed(prompt, true)) {
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        const bool saved = WIFI_STORE.addCredential(selectedSSID, enteredPassword);
        xSemaphoreGive(renderingMutex);
        if (!saved) {
          connectionError = m4WifiKnownNetworkFullCopy();
          saveRejectedAtCap = true;
          updateRequired = true;
          return;
        }
      }
      onComplete(true);
      return;
    }
    if (mappedInput.wasBackGesture()) {
      onComplete(true);
      return;
    }
    return;
  }

  if (state == WifiSelectionState::CONNECTION_FAILED) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasBackGesture() || mappedInput.wasScreenTapped(tx, ty)) {
      if (occupancyDenied) {
        onComplete(false);
        return;
      }
      state = WifiSelectionState::NETWORK_LIST;
      updateRequired = true;
    }
    return;
  }

  if (state != WifiSelectionState::NETWORK_LIST) return;

  if (mappedInput.wasBackGesture()) {
    onComplete(false);
    return;
  }

  // Snapshot the shared list model under the render mutex: the display task
  // paints this same vector/index concurrently.
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  const int snapScanned = static_cast<int>(networks.size());
  const int snapSelected = selectedNetworkIndex;
  xSemaphoreGive(renderingMutex);
  const int itemCount = m4WifiListRowCount(snapScanned);
  const auto layout = TouchHitGeometry::makeWifiNetworkListLayout(renderer.getScreenWidth(), renderer.getScreenHeight(),
                                                                  itemCount);
  const int visible = std::max(1, layout.visibleRows);
  const int pageStart = itemCount == 0 ? 0 : (snapSelected / visible) * visible;
  const int visibleCount = std::max(0, std::min(visible, itemCount - pageStart));

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up && pageStart + visible < itemCount) {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    selectedNetworkIndex = std::min(itemCount - 1, pageStart + visible);
    xSemaphoreGive(renderingMutex);
    updateRequired = true;
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down && pageStart > 0) {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    selectedNetworkIndex = std::max(0, pageStart - visible);
    xSemaphoreGive(renderingMutex);
    updateRequired = true;
    return;
  }

  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    if (layout.refresh.contains(tx, ty)) {
      startWifiScan();
      return;
    }
    int localIndex = -1;
    if (layout.hitRow(tx, ty, visibleCount, localIndex)) {
      const int tapped = pageStart + localIndex;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      selectedNetworkIndex = tapped;
      xSemaphoreGive(renderingMutex);
      selectNetwork(tapped);
    }
    return;
  }

  if (mappedInput.wasScreenTouchDown(tx, ty)) {
    int localIndex = -1;
    if (layout.hitRow(tx, ty, visibleCount, localIndex)) {
      const int hit = pageStart + localIndex;
      if (snapSelected != hit) {
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        selectedNetworkIndex = hit;
        xSemaphoreGive(renderingMutex);
        updateRequired = true;
      }
    }
  }
}

std::string WifiSelectionActivity::getSignalStrengthIndicator(const int32_t rssi) const {
  if (rssi >= -50) return "||||";
  if (rssi >= -60) return "|||";
  if (rssi >= -70) return "||";
  if (rssi >= -80) return "|";
  return ".";
}

std::string WifiSelectionActivity::currentConnectedSsid() const {
  if (!M4QemuNet::staConnected()) return {};
  std::string ssid = WiFi.SSID().c_str();
  if (ssid.empty()) ssid = M4QemuNet::ssidStd();
  return ssid;
}

void WifiSelectionActivity::displayTaskLoop() {
  while (true) {
    if (subActivity || state == WifiSelectionState::PASSWORD_ENTRY ||
        state == WifiSelectionState::HIDDEN_SSID_ENTRY) {
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
      renderConnecting();
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
    case WifiSelectionState::CONFIRM_BT_DISCONNECT:
      renderBtDisconnectConfirm();
      break;
    case WifiSelectionState::PASSWORD_ENTRY:
    case WifiSelectionState::HIDDEN_SSID_ENTRY:
      break;
  }
  renderer.displayBuffer();
}

void WifiSelectionActivity::renderNetworkList() const {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int scannedCount = static_cast<int>(networks.size());
  const int itemCount = m4WifiListRowCount(scannedCount);
  const auto layout = TouchHitGeometry::makeWifiNetworkListLayout(pageWidth, pageHeight, itemCount);
  const std::string connected = currentConnectedSsid();
  const int savedCount = static_cast<int>(WIFI_STORE.getCredentials().size());

  // Settings header semantics, paint-only: brand + battery + title + hairline.
  // Refresh button and row rects below are untouched, so touch still matches.
  M4UiText::draw(renderer, SMALL_FONT_ID, 14, 6, "Murphy M4", true);
  const int battPct = powerManager.getBatteryPercentage() > 100
                          ? 100
                          : static_cast<int>(powerManager.getBatteryPercentage());
  renderer.drawRect(431, 6, 24, 12, true);
  renderer.fillRect(433, 8, (20 * battPct) / 100, 8, true);
  renderer.fillRect(455, 9, 2, 6, true);
  M4UiText::draw(renderer, UI_12_FONT_ID, 14, 30, "Wi-Fi", true, EpdFontFamily::BOLD);
  renderer.drawLine(12, 88, pageWidth - 12, 88, true);
  renderer.drawRoundedRect(layout.refresh.x, layout.refresh.y, layout.refresh.width,
                           layout.refresh.height, 1, 6, true);
  const int refreshTextW = M4UiText::textWidth(renderer, UI_10_FONT_ID, "重新扫描");
  M4UiText::draw(renderer, UI_10_FONT_ID, layout.refresh.x + (layout.refresh.width - refreshTextW) / 2,
                 layout.refresh.y + (layout.refresh.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2,
                 "重新扫描");

  char countText[32];
  snprintf(countText, sizeof(countText), "%d 个网络", scannedCount);
  renderer.drawText(SMALL_FONT_ID, 14, 72, countText);
  char knownBuf[16];
  m4WifiFormatKnownCount(knownBuf, sizeof(knownBuf), savedCount, kM4KnownNetworkCap);
  // Paint-only: end left of the refresh button (was overlapping it).
  const int knownW = M4UiText::textWidth(renderer, SMALL_FONT_ID, knownBuf);
  renderer.drawText(SMALL_FONT_ID, layout.refresh.x - 8 - knownW, 72, knownBuf);

  if (networks.empty()) {
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, pageHeight / 2 - 25, "未找到 Wi-Fi", true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 20, "点按右上角重新扫描");
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 18, "左缘滑动返回");
  }

  const int visible = std::max(1, layout.visibleRows);
  const int pageStart = itemCount == 0 ? 0 : (selectedNetworkIndex / visible) * visible;
  const int visibleCount = std::max(0, std::min(visible, itemCount - pageStart));

  for (int local = 0; local < visibleCount; ++local) {
    const int index = pageStart + local;
    const auto row = layout.rowRect(local);

    // Settings grammar: dividers for all rows, thin rounded outline for the
    // selected row (same rowRect geometry, so touch is unchanged).
    if (index == selectedNetworkIndex) {
      renderer.drawRoundedRect(row.x + 1, row.y + 1, row.width - 2, row.height - 2, 1, 6, true);
      renderer.fillRectStipple(row.x + 5, row.y + 5, row.width - 10, row.height - 10);
    }
    renderer.drawLine(row.x + 8, row.y + row.height - 1, row.x + row.width - 8,
                      row.y + row.height - 1, true);
    const int chevX = row.x + row.width - 26;
    const int chevCy = row.y + row.height / 2;
    renderer.drawLine(chevX, chevCy - 8, chevX + 10, chevCy, 2, true);
    renderer.drawLine(chevX + 10, chevCy, chevX, chevCy + 8, 2, true);

    if (m4WifiListRowIsHidden(index, scannedCount)) {
      M4UiText::draw(renderer, UI_10_FONT_ID, row.x + 12, row.y + 10, "加入隐藏网络", true,
                     EpdFontFamily::BOLD);
      renderer.drawText(SMALL_FONT_ID, row.x + 12, row.y + 42, "输入名称与密码");
      continue;
    }

    const auto& network = networks[static_cast<size_t>(index)];
    const bool isCurrent = !connected.empty() && network.ssid == connected;
    M4UiText::draw(renderer, UI_10_FONT_ID, row.x + 12, row.y + 10, network.ssid.c_str(),
                   network.hasSavedPassword || isCurrent, EpdFontFamily::BOLD);
    if (isCurrent) {
      renderer.drawText(SMALL_FONT_ID, row.x + row.width - 52, row.y + 12, "✓");
    }

    std::string meta = "信号 " + getSignalStrengthIndicator(network.rssi);
    if (network.hasSavedPassword) meta += "  已保存";
    if (network.isEncrypted) meta += "  加密";
    if (isCurrent) meta += "  已连接";
    renderer.drawText(SMALL_FONT_ID, row.x + 12, row.y + 42, meta.c_str());
  }

  if (pageStart > 0) renderer.drawText(SMALL_FONT_ID, pageWidth - 22, layout.rowTop - 12, "^");
  if (pageStart + visibleCount < itemCount) renderer.drawText(SMALL_FONT_ID, pageWidth - 22, pageHeight - 16, "v");
}

void WifiSelectionActivity::renderConnecting() const {
  const int pageHeight = renderer.getScreenHeight();
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, 30, "Wi-Fi", true, EpdFontFamily::BOLD);

  if (state == WifiSelectionState::SCANNING) {
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, pageHeight / 2 - 20, "正在扫描网络…", true,
                           EpdFontFamily::BOLD);
  } else {
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, pageHeight / 2 - 50, "正在连接…", true,
                           EpdFontFamily::BOLD);
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight / 2, selectedSSID.c_str());
  }
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 18, "左缘滑动取消");
}

void WifiSelectionActivity::renderConnected() const {
  const int pageHeight = renderer.getScreenHeight();
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, pageHeight / 2 - 45, "Wi-Fi 已连接", true,
                         EpdFontFamily::BOLD);
  M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight / 2, selectedSSID.c_str());
  const std::string ipInfo = "IP: " + connectedIP;
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 38, ipInfo.c_str());
  if (saveRejectedAtCap) {
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight / 2 + 76, "已保存网络已满 8/8，本次未保存");
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 110, "点按屏幕继续");
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 18, "左缘滑动返回");
    return;
  }
  M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight / 2 + 76, "是否保存此 Wi-Fi 密码?");
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 110, "点按屏幕保存");
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 18, "左缘滑动跳过（本次仍可使用）");
}

void WifiSelectionActivity::renderBtDisconnectConfirm() const {
  const int pageHeight = renderer.getScreenHeight();
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, pageHeight / 2 - 40, "确认", true, EpdFontFamily::BOLD);
  M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight / 2, "连接 Wi-Fi 会断开蓝牙翻页");
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 48, "点按屏幕确认");
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 18, "左缘滑动取消");
}

void WifiSelectionActivity::renderConnectionFailed() const {
  const int pageHeight = renderer.getScreenHeight();
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, pageHeight / 2 - 55, "连接失败", true, EpdFontFamily::BOLD);
  M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight / 2 - 5, connectionError.c_str());
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 48, "点按屏幕返回 Wi-Fi 列表");
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 18, "左缘滑动返回");
}
