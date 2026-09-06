#include "WifiSelectionActivity.h"

#include <GfxRenderer.h>
#include <WiFi.h>

#include <algorithm>
#include <map>

#include "BluetoothHIDManager.h"
#include "I18n.h"
#include "MappedInputManager.h"
#include "WifiCredentialStore.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "qemu/M4QemuNet.h"
#include "util/M4UiText.h"
#include "util/TouchUiGeometry.h"

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
  networks.clear();
  state = WifiSelectionState::SCANNING;
  selectedSSID.clear();
  connectedIP.clear();
  connectionError.clear();
  enteredPassword.clear();
  usedSavedPassword = false;
  failureTracker.reset();

  uint8_t mac[6];
  WiFi.macAddress(mac);
  char macStr[32];
  snprintf(macStr, sizeof(macStr), "MAC: %02x-%02x-%02x-%02x-%02x-%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  cachedMacAddress = std::string(macStr);

  updateRequired = true;
  xTaskCreate(&WifiSelectionActivity::taskTrampoline, "WifiSelectionTask", 4096, this, 1, &displayTaskHandle);
  startWifiScan();
}

void WifiSelectionActivity::onExit() {
  Activity::onExit();
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
  state = WifiSelectionState::SCANNING;
  networks.clear();
  selectedNetworkIndex = 0;
  updateRequired = true;

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
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

  networks.clear();
  for (const auto& pair : uniqueNetworks) networks.push_back(pair.second);
  std::sort(networks.begin(), networks.end(),
            [](const WifiNetworkInfo& a, const WifiNetworkInfo& b) { return a.rssi > b.rssi; });
  std::stable_sort(networks.begin(), networks.end(), [](const WifiNetworkInfo& a, const WifiNetworkInfo& b) {
    return a.hasSavedPassword && !b.hasSavedPassword;
  });

  WiFi.scanDelete();
  state = WifiSelectionState::NETWORK_LIST;
  selectedNetworkIndex = 0;
  updateRequired = true;
}

void WifiSelectionActivity::selectNetwork(const int index) {
  if (index < 0 || index >= static_cast<int>(networks.size())) return;

  const auto& network = networks[index];
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

void WifiSelectionActivity::attemptConnection() {
  state = WifiSelectionState::CONNECTING;
  connectionStartTime = millis();
  connectedIP.clear();
  connectionError.clear();
  failureTracker.reset();
  updateRequired = true;

  try {
    auto& btMgr = BluetoothHIDManager::getInstance();
    if (btMgr.isEnabled()) btMgr.disable();
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
  if (state != WifiSelectionState::CONNECTING) return;

  const wl_status_t status = WiFi.status();
  if (M4QemuNet::staConnected()) {
    const IPAddress ip = WiFi.localIP();
    char ipStr[16];
    snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    connectedIP = ipStr;

    configTime(8 * 3600, 0, "pool.ntp.org", "time.cloudflare.com");

    if (!usedSavedPassword && !enteredPassword.empty()) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      const bool saved = WIFI_STORE.addCredential(selectedSSID, enteredPassword);
      xSemaphoreGive(renderingMutex);
      if (!saved) {
        Serial.printf("[%lu] [WIFI] Connected, but could not save Wi-Fi for %s\n", millis(), selectedSSID.c_str());
      }
    }
    onComplete(true);
    return;
  }

  if (failureTracker.authenticationFailed()) {
    if (usedSavedPassword) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      const bool removed = WIFI_STORE.removeCredential(selectedSSID);
      xSemaphoreGive(renderingMutex);
      const auto network = std::find_if(networks.begin(), networks.end(),
                                        [this](const WifiNetworkInfo& net) { return net.ssid == selectedSSID; });
      if (removed && network != networks.end()) network->hasSavedPassword = false;
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
    pumpSubActivityFrame();
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

  if (state == WifiSelectionState::PASSWORD_ENTRY) {
    attemptConnection();
    return;
  }

  if (state == WifiSelectionState::CONNECTED) {
    onComplete(true);
    return;
  }

  if (state == WifiSelectionState::CONNECTION_FAILED) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasBackGesture() || mappedInput.wasScreenTapped(tx, ty)) {
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

  const int itemCount = static_cast<int>(networks.size());
  const auto layout = TouchHitGeometry::makeWifiNetworkListLayout(renderer.getScreenWidth(), renderer.getScreenHeight(),
                                                                  itemCount);
  const int visible = std::max(1, layout.visibleRows);
  const int pageStart = itemCount == 0 ? 0 : (selectedNetworkIndex / visible) * visible;
  const int visibleCount = std::max(0, std::min(visible, itemCount - pageStart));

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up && pageStart + visible < itemCount) {
    selectedNetworkIndex = std::min(itemCount - 1, pageStart + visible);
    updateRequired = true;
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down && pageStart > 0) {
    selectedNetworkIndex = std::max(0, pageStart - visible);
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
      selectedNetworkIndex = pageStart + localIndex;
      selectNetwork(selectedNetworkIndex);
    }
    return;
  }

  if (mappedInput.wasScreenTouchDown(tx, ty)) {
    int localIndex = -1;
    if (layout.hitRow(tx, ty, visibleCount, localIndex)) {
      const int hit = pageStart + localIndex;
      if (selectedNetworkIndex != hit) {
        selectedNetworkIndex = hit;
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

void WifiSelectionActivity::displayTaskLoop() {
  while (true) {
    if (subActivity || state == WifiSelectionState::PASSWORD_ENTRY) {
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
    case WifiSelectionState::PASSWORD_ENTRY:
      break;
  }
  renderer.displayBuffer();
}

void WifiSelectionActivity::renderNetworkList() const {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int itemCount = static_cast<int>(networks.size());
  const auto layout = TouchHitGeometry::makeWifiNetworkListLayout(pageWidth, pageHeight, itemCount);

  M4UiText::draw(renderer, UI_12_FONT_ID, 14, 30, "Wi-Fi", true, EpdFontFamily::BOLD);
  renderer.drawRect(layout.refresh.x, layout.refresh.y, layout.refresh.width, layout.refresh.height);
  const int refreshTextW = M4UiText::textWidth(renderer, UI_10_FONT_ID, "重新扫描");
  M4UiText::draw(renderer, UI_10_FONT_ID, layout.refresh.x + (layout.refresh.width - refreshTextW) / 2,
                 layout.refresh.y + (layout.refresh.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2,
                 "重新扫描");

  char countText[32];
  snprintf(countText, sizeof(countText), "%d 个网络", itemCount);
  renderer.drawText(SMALL_FONT_ID, 14, 72, countText);

  if (networks.empty()) {
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, pageHeight / 2 - 25, "未找到 Wi-Fi", true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 20, "点按右上角重新扫描");
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 18, "左缘滑动返回");
    return;
  }

  const int visible = std::max(1, layout.visibleRows);
  const int pageStart = (selectedNetworkIndex / visible) * visible;
  const int visibleCount = std::min(visible, itemCount - pageStart);

  for (int local = 0; local < visibleCount; ++local) {
    const int index = pageStart + local;
    const auto& network = networks[index];
    const auto row = layout.rowRect(local);

    if (index == selectedNetworkIndex) {
      renderer.fillRectDither(row.x + 1, row.y + 1, row.width - 2, row.height - 2, LightGray);
    }
    renderer.drawRect(row.x, row.y, row.width, row.height);

    M4UiText::draw(renderer, UI_10_FONT_ID, row.x + 12, row.y + 10, network.ssid.c_str(),
                   network.hasSavedPassword, EpdFontFamily::BOLD);

    std::string meta = "信号 " + getSignalStrengthIndicator(network.rssi);
    if (network.hasSavedPassword) meta += "  已保存";
    if (network.isEncrypted) meta += "  加密";
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
}

void WifiSelectionActivity::renderConnectionFailed() const {
  const int pageHeight = renderer.getScreenHeight();
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, pageHeight / 2 - 55, "连接失败", true, EpdFontFamily::BOLD);
  M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight / 2 - 5, connectionError.c_str());
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 48, "点按屏幕返回 Wi-Fi 列表");
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 18, "左缘滑动返回");
}