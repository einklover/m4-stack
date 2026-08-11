#include "SimpleBluetoothActivity.h"

#include <algorithm>
#include <GfxRenderer.h>
#include <BluetoothHIDManager.h>
#include <CrossPointSettings.h>

#include "I18n.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"
#include "util/M4ListTouchPolicy.h"
#include "util/M4TouchListMetrics.h"

#define GUI UITheme::getInstance().getTheme()

void SimpleBluetoothActivity::taskTrampoline(void* param) {
  auto* self = static_cast<SimpleBluetoothActivity*>(param);
  self->displayTaskLoop();
}

void SimpleBluetoothActivity::displayTaskLoop() {
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

void SimpleBluetoothActivity::onEnter() {
  Activity::onEnter();
  
  renderingMutex = xSemaphoreCreateMutex();
  
  // 获取蓝牙管理器
  btMgr = &BluetoothHIDManager::getInstance();
  
  // 如果蓝牙未启用，尝试启用
  if (!btMgr->isEnabled()) {
    Serial.printf("[%lu] [BT-UI] Enabling Bluetooth...\n", millis());
    if (!btMgr->enable()) {
      Serial.printf("[%lu] [BT-UI] Failed to enable Bluetooth\n", millis());
    }
    delay(500);
  }
  
  selectedIndex = 0;
  devices.clear();
  selectedDeviceAddress = "";
  connectionError = "";
  scanJustStarted = false;  // 初始化扫描标记
  state = BtPageState::MAIN_MENU;  // 从主菜单开始
  
  // 重置按键映射状态
  keyMappingSelectedSlot = 0;
  keyActionSelectedIndex = 0;
  learntKeycode = 0x00;
  
  updateRequired = true;
  
  // 创建渲染任务
  xTaskCreate(
    &SimpleBluetoothActivity::taskTrampoline,
    "SimpleBtTask",
    4096,
    this,
    1,
    &displayTaskHandle
  );
}

void SimpleBluetoothActivity::onExit() {
  Activity::onExit();
  
  Serial.printf("[%lu] [BT-UI] Exiting\n", millis());
  
  // 停止扫描
  if (btMgr && btMgr->isScanning()) {
    btMgr->stopScan();
  }
  
  // 等待任务结束
  if (renderingMutex) {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    if (displayTaskHandle) {
      vTaskDelete(displayTaskHandle);
      displayTaskHandle = nullptr;
    }
    xSemaphoreGive(renderingMutex);
    vSemaphoreDelete(renderingMutex);
    renderingMutex = nullptr;
  }
}

void SimpleBluetoothActivity::loop() {
  // 处理蓝牙事件
  if (btMgr) {
    btMgr->processInputEvents();
  }

  // Touch edge-back mirrors physical Back on this screen.
  if (mappedInput.hasTouch() && mappedInput.wasBackGesture()) {
    if (state == BtPageState::DEVICE_LIST) {
      state = BtPageState::MAIN_MENU;
      selectedIndex = 0;
      if (btMgr && btMgr->isScanning()) btMgr->stopScan();
      updateRequired = true;
      return;
    } else if (state == BtPageState::KEY_MAPPING) {
      state = BtPageState::MAIN_MENU;
      selectedIndex = 2;
      updateRequired = true;
      return;
    } else if (state == BtPageState::KEY_LEARNING) {
      if (btMgr) btMgr->consumeLastKeycode();
      state = BtPageState::KEY_MAPPING;
      updateRequired = true;
      return;
    } else if (state == BtPageState::KEY_ACTION_SELECT) {
      if (btMgr) btMgr->consumeLastKeycode();
      state = BtPageState::KEY_LEARNING;
      updateRequired = true;
      return;
    } else if (state != BtPageState::CONNECTED && state != BtPageState::CONNECTION_FAILED) {
      onComplete();
      return;
    }
  }
  
  // 返回键路由
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (state == BtPageState::DEVICE_LIST) {
      state = BtPageState::MAIN_MENU;
      selectedIndex = 0;
      if (btMgr && btMgr->isScanning()) btMgr->stopScan();
      updateRequired = true;
      return;
    } else if (state == BtPageState::KEY_MAPPING) {
      state = BtPageState::MAIN_MENU;
      selectedIndex = 2;  // 选回“按键映射”
      updateRequired = true;
      return;
    } else if (state == BtPageState::KEY_LEARNING) {
      if (btMgr) btMgr->consumeLastKeycode();
      state = BtPageState::KEY_MAPPING;
      updateRequired = true;
      return;
    } else if (state == BtPageState::KEY_ACTION_SELECT) {
      if (btMgr) btMgr->consumeLastKeycode();
      state = BtPageState::KEY_LEARNING;
      updateRequired = true;
      return;
    } else if (state == BtPageState::CONNECTED || state == BtPageState::CONNECTION_FAILED) {
      // 在各自的处理函数中处理返回键
      return;
    } else {
      // 主菜单返回 = 退出
      onComplete();
      return;
    }
  }
  
  // 检测扫描是否完成
  if (state == BtPageState::DEVICE_LIST && btMgr && !btMgr->isScanning()) {
    // 关键修复：如果扫描刚启动，不立即更新列表（等待扫描真正开始）
    if (scanJustStarted) {
      scanJustStarted = false;  // 清除标记
      return;  // 等待下一次loop
    }
    // 只有在扫描真正完成后才更新设备列表
    updateDeviceList();
  }
  
  // 如果正在扫描且设备列表为空，不要立即显示"未发现设备"
  // 等待扫描进行一段时间后再更新UI
  if (state == BtPageState::DEVICE_LIST && btMgr && btMgr->isScanning()) {
    // 扫描中，定期更新设备列表以显示已发现的设备
    static unsigned long lastUpdate = 0;
    if (millis() - lastUpdate > 2000) {  // 每2秒更新一次
      updateDeviceList();
      lastUpdate = millis();
      updateRequired = true;
    }
  }
  
  // 路由到各状态的输入处理
  if (state == BtPageState::MAIN_MENU) {
    handleMainMenuInput();
  } else if (state == BtPageState::DEVICE_LIST) {
    handleDeviceListInput();
  } else if (state == BtPageState::CONNECTED) {
    handleConnectedInput();
  } else if (state == BtPageState::CONNECTION_FAILED) {
    handleConnectionFailedInput();
  } else if (state == BtPageState::KEY_MAPPING) {
    handleKeyMappingInput();
  } else if (state == BtPageState::KEY_LEARNING) {
    handleKeyLearningInput();
  } else if (state == BtPageState::KEY_ACTION_SELECT) {
    handleKeyActionInput();
  }
}

void SimpleBluetoothActivity::startScan() {
  if (!btMgr) return;
  
  devices.clear();
  selectedIndex = 0;
  state = BtPageState::DEVICE_LIST;  // 直接进入设备列表
  scanJustStarted = true;  // 关键修复：标记扫描刚启动
  updateRequired = true;
  
  btMgr->startScan(15000);
  Serial.printf("[%lu] [BT-UI] Scan started\n", millis());
}

void SimpleBluetoothActivity::connectToDevice(int index) {
  if (index < 0 || index >= static_cast<int>(devices.size())) return;
  
  state = BtPageState::CONNECTING;
  selectedDeviceAddress = devices[index].address;
  updateRequired = true;
  
  Serial.printf("[%lu] [BT-UI] Connecting to %s\n", millis(), devices[index].name.c_str());
  
  // 在后台任务中连接
  xTaskCreate(
    [](void* param) {
      auto* self = static_cast<SimpleBluetoothActivity*>(param);
      
      bool success = self->btMgr->connectToDeviceWithRetries(self->selectedDeviceAddress, 3);
      
      xSemaphoreTake(self->renderingMutex, portMAX_DELAY);
      if (success) {
        self->state = BtPageState::CONNECTED;
        Serial.printf("[%lu] [BT-UI] Connected successfully\n", millis());
      } else {
        self->state = BtPageState::CONNECTION_FAILED;
        self->connectionError = self->btMgr->lastError;
        Serial.printf("[%lu] [BT-UI] Connection failed: %s\n", millis(), self->connectionError.c_str());
      }
      self->updateRequired = true;
      xSemaphoreGive(self->renderingMutex);
      
      vTaskDelete(nullptr);
    },
    "BtConnectTask",
    4096,
    this,
    1,
    nullptr
  );
}

void SimpleBluetoothActivity::updateDeviceList() {
  if (!btMgr) return;
  
  devices.clear();
  for (const auto& dev : btMgr->getDiscoveredDevices()) {
    // 过滤掉Unknown和空名称
    if (!dev.name.empty() && dev.name != "Unknown") {
      devices.push_back(dev);
    }
  }
}

void SimpleBluetoothActivity::render() {
  renderer.clearScreen();
  
  switch (state) {
    case BtPageState::MAIN_MENU:
      renderMainMenu();
      break;
    case BtPageState::DEVICE_LIST:
      renderDeviceList();
      break;
    case BtPageState::CONNECTING:
      renderConnecting();
      break;
    case BtPageState::CONNECTED:
      renderConnected();
      break;
    case BtPageState::CONNECTION_FAILED:
      renderConnectionFailed();
      break;
    case BtPageState::KEY_MAPPING:
      renderKeyMapping();
      break;
    case BtPageState::KEY_LEARNING:
      renderKeyLearning();
      break;
    case BtPageState::KEY_ACTION_SELECT:
      renderKeyActionSelect();
      break;
  }
  
  renderer.displayBuffer();
}

void SimpleBluetoothActivity::renderScanning() {
  const auto pageHeight = renderer.getScreenHeight();
  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto centerY = (pageHeight - lineHeight * 2) / 2;
  
  // 更新设备列表
  updateDeviceList();
  
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, centerY - 30, L(Str::kScanBluetooth), true, EpdFontFamily::BOLD);
  M4UiText::drawCentered(renderer, UI_10_FONT_ID, centerY, L(Str::kScanningPleaseWait));
  
  char countStr[32];
  snprintf(countStr, sizeof(countStr), L(Str::kFoundDevices), devices.size());
  renderer.drawCenteredText(SMALL_FONT_ID, centerY + lineHeight + 10, countStr);
  
  // 按钮提示
  const auto labels = mappedInput.mapLabels(L(Str::kBack), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
}

void SimpleBluetoothActivity::renderDeviceList() {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  
  // 更新设备列表
  updateDeviceList();
  
  // 标题
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, 15, L(Str::kBluetoothTitle), true, EpdFontFamily::BOLD);
  
  // 状态行
  char statusStr[64];
  if (btMgr && btMgr->isScanning()) {
    snprintf(statusStr, sizeof(statusStr), L(Str::kScanningDevices), devices.size());
  } else {
    snprintf(statusStr, sizeof(statusStr), L(Str::kTotalDevices), devices.size());
  }
  renderer.drawText(SMALL_FONT_ID, 20, 40, statusStr);
  
  // 已连接设备提示
  int startY = 70;  // 设备列表起始Y坐标
  auto connectedDevices = btMgr ? btMgr->getConnectedDevices() : std::vector<std::string>();
  if (!connectedDevices.empty() && !selectedDeviceAddress.empty()) {
    std::string devName = L(Str::kBluetoothTitle);
    for (const auto& d : devices) {
      if (d.address == selectedDeviceAddress) {
        devName = d.name;
        break;
      }
    }
    
    char connStr[64];
    snprintf(connStr, sizeof(connStr), L(Str::kConnected), devName.c_str());
    M4UiText::draw(renderer, UI_10_FONT_ID, 20, 60, connStr, true);
    
    // 设备列表区域需要下移，避免与已连接提示重叠
    startY = 85;
  }
  const int lineHeight = M4TouchListMetrics::listRowHeight(mappedInput.hasTouch());
  const int maxVisibleDevices = std::max(1, (pageHeight - startY - 50) / lineHeight);
  
  if (devices.empty()) {
    // 无设备提示
    const auto height = renderer.getLineHeight(UI_10_FONT_ID);
    const auto top = startY + (maxVisibleDevices * lineHeight - height * 2) / 2;
    
    // 检查是否正在扫描
    bool isScanning = btMgr && btMgr->isScanning();
    
    if (isScanning) {
      // 扫描中：显示扫描提示
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, top, L(Str::kScanningBluetooth));
      renderer.drawCenteredText(SMALL_FONT_ID, top + height + 10, L(Str::kPleaseWait));
    } else {
      // 扫描完成：显示未发现设备
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, top, L(Str::kNoBluetoothFound));
      renderer.drawCenteredText(SMALL_FONT_ID, top + height + 10, L(Str::kPressConfirmRescan));
    }
  } else {
    // 计算滚动偏移
    int scrollOffset = 0;
    if (selectedIndex >= maxVisibleDevices) {
      scrollOffset = selectedIndex - maxVisibleDevices + 1;
    }
    
    // 绘制设备列表
    int displayIndex = 0;
    for (size_t i = scrollOffset; i < devices.size() && displayIndex < maxVisibleDevices; i++, displayIndex++) {
      const int deviceY = startY + displayIndex * lineHeight;
      const auto& device = devices[i];
      
      // 选中指示器
      if (static_cast<int>(i) == selectedIndex) {
        M4UiText::draw(renderer, UI_10_FONT_ID, 5, deviceY, ">");
      }
      
      // 设备名称
      std::string displayName = device.name;
      if (displayName.length() > 28) {
        displayName = displayName.substr(0, 25) + "...";
      }
      
      // 检查是否已连接
      bool connected = btMgr && btMgr->isConnected(device.address);
      if (connected) {
        displayName = "✓ " + displayName;
      }
      
      M4UiText::draw(renderer, UI_10_FONT_ID, 20, deviceY, displayName.c_str());
      
      // 信号强度
      char rssiStr[16];
      snprintf(rssiStr, sizeof(rssiStr), "%d", device.rssi);
      M4UiText::draw(renderer, UI_10_FONT_ID, pageWidth - 60, deviceY, rssiStr);
    }
    
    // 滚动指示器
    if (scrollOffset > 0) {
      renderer.drawText(SMALL_FONT_ID, pageWidth - 15, startY - 10, "^");
    }
    if (scrollOffset + maxVisibleDevices < static_cast<int>(devices.size())) {
      renderer.drawText(SMALL_FONT_ID, pageWidth - 15, startY + maxVisibleDevices * lineHeight, "v");
    }
  }

  // 按钮提示
  if (!devices.empty()) {
    const auto labels = mappedInput.mapLabels(L(Str::kBack), L(Str::kConnect), "\u2191", "\u2193");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  } else {
    const auto labels = mappedInput.mapLabels(L(Str::kBack), L(Str::kScan), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  }
}

void SimpleBluetoothActivity::renderConnecting() {
  const auto pageHeight = renderer.getScreenHeight();
  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto centerY = (pageHeight - lineHeight * 3) / 2;
  
  // 查找设备名称
  std::string devName = selectedDeviceAddress;
  for (const auto& d : devices) {
    if (d.address == selectedDeviceAddress) {
      devName = d.name;
      break;
    }
  }
  
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, centerY - 40, L(Str::kConnecting), true, EpdFontFamily::BOLD);
  M4UiText::drawCentered(renderer, UI_10_FONT_ID, centerY, devName.c_str());
  renderer.drawCenteredText(SMALL_FONT_ID, centerY + lineHeight + 10, L(Str::kPleaseWait));
}

void SimpleBluetoothActivity::renderConnected() {
  const auto pageHeight = renderer.getScreenHeight();
  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto centerY = (pageHeight - lineHeight * 3) / 2;
  
  // 查找设备名称
  std::string devName = selectedDeviceAddress;
  for (const auto& d : devices) {
    if (d.address == selectedDeviceAddress) {
      devName = d.name;
      break;
    }
  }
  
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, centerY - 40, L(Str::kConnectSuccess), true, EpdFontFamily::BOLD);
  M4UiText::drawCentered(renderer, UI_10_FONT_ID, centerY, (std::string(L(Str::kDeviceLabel)) + devName).c_str());
  renderer.drawCenteredText(SMALL_FONT_ID, centerY + lineHeight + 10, L(Str::kNowCanUseBT));

  const auto labels = mappedInput.mapLabels(L(Str::kBack), L(Str::kDone), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
}

void SimpleBluetoothActivity::renderConnectionFailed() {
  const auto pageHeight = renderer.getScreenHeight();
  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto centerY = (pageHeight - lineHeight * 3) / 2;
  
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, centerY - 40, L(Str::kConnectFailed), true, EpdFontFamily::BOLD);
  
  std::string errorMsg = connectionError.empty() ? L(Str::kUnknownError) : connectionError;
  M4UiText::drawCentered(renderer, UI_10_FONT_ID, centerY, errorMsg.c_str());
  renderer.drawCenteredText(SMALL_FONT_ID, centerY + lineHeight + 10, L(Str::kPressAnyKeyBack));
}

// ============ 主菜单 ============

void SimpleBluetoothActivity::renderMainMenu() {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, 15, L(Str::kBluetoothTitle), true, EpdFontFamily::BOLD);

  // 状态行
  std::string statusLine;
  if (btMgr) {
    if (btMgr->isEnabled()) {
      auto connDevices = btMgr->getConnectedDevices();
      char buf[64];
      snprintf(buf, sizeof(buf), L(Str::kEnabled), connDevices.size());
      statusLine = buf;
    } else {
      statusLine = L(Str::kDisabled);
    }
  } else {
    statusLine = L(Str::kBluetoothError);
  }
  renderer.drawText(SMALL_FONT_ID, 20, 45, statusLine.c_str());
  
  // 菜单项（触屏加大行高）
  constexpr int startY = 90;
  const int lineHeight = M4TouchListMetrics::listRowHeight(mappedInput.hasTouch());
  const int rowFont = mappedInput.hasTouch() ? UI_12_FONT_ID : UI_10_FONT_ID;
  const char* items[] = {
    btMgr && btMgr->isEnabled() ? L(Str::kDisableBluetooth) : L(Str::kEnableBluetooth),
    L(Str::kScanDevices),
    L(Str::kKeyMapping)
  };
  
  for (int i = 0; i < 3; i++) {
    const int itemY = startY + i * lineHeight;
    if (i == selectedIndex) {
      if (mappedInput.hasTouch()) {
        renderer.fillRect(8, itemY, renderer.getScreenWidth() - 16, lineHeight - 4, true);
      } else {
        M4UiText::draw(renderer, UI_10_FONT_ID, 5, itemY, ">");
      }
    }
    renderer.drawText(rowFont, 25, itemY + (mappedInput.hasTouch() ? 10 : 0), items[i],
                      !(mappedInput.hasTouch() && i == selectedIndex));
  }
  
  const auto labels = mappedInput.mapLabels(L(Str::kBack), L(Str::kSelect), "\u2191", "\u2193");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
}

void SimpleBluetoothActivity::handleMainMenuInput() {
  constexpr int kMenuCount = 3;
  constexpr int startY = 90;
  const int lineHeight = M4TouchListMetrics::listRowHeight(mappedInput.hasTouch());

  auto activateMain = [this]() {
    if (!btMgr) {
      updateRequired = true;
      return;
    }
    if (selectedIndex == 0) {
      if (btMgr->isEnabled()) {
        btMgr->disable();
      } else {
        btMgr->enable();
        delay(500);
      }
      updateRequired = true;
    } else if (selectedIndex == 1) {
      if (btMgr->isEnabled()) {
        startScan();
      }
      updateRequired = true;
    } else if (selectedIndex == 2) {
      state = BtPageState::KEY_MAPPING;
      keyMappingSelectedSlot = 0;
      updateRequired = true;
    }
  };

  if (mappedInput.hasTouch()) {
    M4ListTouchPolicy::Event te{};
    const auto sw = mappedInput.wasSwipe();
    if (sw == MappedInputManager::SwipeDir::Up) te.swipe = M4ListTouchPolicy::Swipe::Up;
    else if (sw == MappedInputManager::SwipeDir::Down) te.swipe = M4ListTouchPolicy::Swipe::Down;
    int dx = 0, dy = 0, tx = 0, ty = 0;
    te = M4ListTouchPolicy::mergeFrame(false, te.swipe, mappedInput.wasScreenTouchDown(dx, dy), dx, dy,
                                       mappedInput.wasScreenTapped(tx, ty), tx, ty);
    M4ListTouchPolicy::ListLayout layout;
    layout.listTop = startY;
    layout.listHeight = kMenuCount * lineHeight;
    layout.rowStep = lineHeight;
    layout.itemCount = kMenuCount;
    layout.selectedIndex = selectedIndex;
    layout.maxVisible = kMenuCount;
    int hit = -1;
    const auto act = M4ListTouchPolicy::resolveList(te, layout, hit);
    if (act == M4ListTouchPolicy::Action::Select && hit >= 0) {
      if (selectedIndex != hit) {
        selectedIndex = hit;
        updateRequired = true;
      }
      return;
    }
    if (act == M4ListTouchPolicy::Action::Activate && hit >= 0) {
      selectedIndex = hit;
      activateMain();
      return;
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    selectedIndex = (selectedIndex > 0) ? selectedIndex - 1 : 2;
    updateRequired = true;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    selectedIndex = (selectedIndex < 2) ? selectedIndex + 1 : 0;
    updateRequired = true;
  }
  
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    activateMain();
  }
}

// ============ 设备列表 ============

void SimpleBluetoothActivity::handleDeviceListInput() {
  if (!btMgr) return;
  
  int menuItems = static_cast<int>(devices.size()) + 1;  // +1 for Refresh
  int maxIndex = menuItems - 1;
  const int pageHeight = renderer.getScreenHeight();
  constexpr int startY = 70;
  const int lineHeight = M4TouchListMetrics::listRowHeight(mappedInput.hasTouch());
  const int maxVisible = std::max(1, (pageHeight - startY - 50) / lineHeight);

  auto activateDevice = [this]() {
    if (selectedIndex == static_cast<int>(devices.size())) {
      startScan();
      return;
    }
    if (selectedIndex >= 0 && selectedIndex < static_cast<int>(devices.size())) {
      connectToDevice(selectedIndex);
    }
  };

  if (mappedInput.hasTouch() && menuItems > 0) {
    M4ListTouchPolicy::Event te{};
    const auto sw = mappedInput.wasSwipe();
    if (sw == MappedInputManager::SwipeDir::Up) te.swipe = M4ListTouchPolicy::Swipe::Up;
    else if (sw == MappedInputManager::SwipeDir::Down) te.swipe = M4ListTouchPolicy::Swipe::Down;
    int dx = 0, dy = 0, tx = 0, ty = 0;
    te = M4ListTouchPolicy::mergeFrame(false, te.swipe, mappedInput.wasScreenTouchDown(dx, dy), dx, dy,
                                       mappedInput.wasScreenTapped(tx, ty), tx, ty);
    M4ListTouchPolicy::ListLayout layout;
    layout.listTop = startY;
    layout.listHeight = maxVisible * lineHeight;
    layout.rowStep = lineHeight;
    layout.itemCount = menuItems;
    layout.selectedIndex = selectedIndex;
    layout.maxVisible = maxVisible;
    int hit = -1;
    const auto act = M4ListTouchPolicy::resolveList(te, layout, hit);
    if (act == M4ListTouchPolicy::Action::PageDown || act == M4ListTouchPolicy::Action::PageUp) {
      selectedIndex = M4ListTouchPolicy::applyPage(selectedIndex, menuItems, maxVisible,
                                                   act == M4ListTouchPolicy::Action::PageDown);
      updateRequired = true;
      return;
    }
    if (act == M4ListTouchPolicy::Action::Select && hit >= 0) {
      if (selectedIndex != hit) {
        selectedIndex = hit;
        updateRequired = true;
      }
      return;
    }
    if (act == M4ListTouchPolicy::Action::Activate && hit >= 0) {
      selectedIndex = hit;
      activateDevice();
      return;
    }
  }
  
  // 支持侧边按钮和正面左/右按钮
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    selectedIndex = (selectedIndex > 0) ? selectedIndex - 1 : maxIndex;
    updateRequired = true;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    selectedIndex = (selectedIndex < maxIndex) ? selectedIndex + 1 : 0;
    updateRequired = true;
  }
  
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    activateDevice();
  }
}

// ============ 按键映射辅助函数 ============

static uint8_t getBtKeyCode(int slot) {
  switch (slot) {
    case 0: return SETTINGS.btKey1Code;
    case 1: return SETTINGS.btKey2Code;
    case 2: return SETTINGS.btKey3Code;
    case 3: return SETTINGS.btKey4Code;
    case 4: return SETTINGS.btKey5Code;
    case 5: return SETTINGS.btKey6Code;
    default: return 0x00;
  }
}

static uint8_t getBtKeyAction(int slot) {
  switch (slot) {
    case 0: return SETTINGS.btKey1Action;
    case 1: return SETTINGS.btKey2Action;
    case 2: return SETTINGS.btKey3Action;
    case 3: return SETTINGS.btKey4Action;
    case 4: return SETTINGS.btKey5Action;
    case 5: return SETTINGS.btKey6Action;
    default: return 0xFF;
  }
}

static void setBtKeyMapping(int slot, uint8_t code, uint8_t action) {
  switch (slot) {
    case 0: SETTINGS.btKey1Code = code; SETTINGS.btKey1Action = action; break;
    case 1: SETTINGS.btKey2Code = code; SETTINGS.btKey2Action = action; break;
    case 2: SETTINGS.btKey3Code = code; SETTINGS.btKey3Action = action; break;
    case 3: SETTINGS.btKey4Code = code; SETTINGS.btKey4Action = action; break;
    case 4: SETTINGS.btKey5Code = code; SETTINGS.btKey5Action = action; break;
    case 5: SETTINGS.btKey6Code = code; SETTINGS.btKey6Action = action; break;
  }
}

// BTN_NAMES and ACTION_LABELS are defined inside render functions to support runtime language switching
static const uint8_t ACTION_TO_BTN[] = {
  4, 5, 2, 3, 1, 0, 0xFF  // BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_CONFIRM, BTN_BACK
};
static constexpr int ACTION_COUNT = 7;

void SimpleBluetoothActivity::handleKeyMappingInput() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    keyMappingSelectedSlot = (keyMappingSelectedSlot > 0) ? keyMappingSelectedSlot - 1 : 5;
    updateRequired = true;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    keyMappingSelectedSlot = (keyMappingSelectedSlot < 5) ? keyMappingSelectedSlot + 1 : 0;
    updateRequired = true;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (btMgr) btMgr->consumeLastKeycode();
    state = BtPageState::KEY_LEARNING;
    updateRequired = true;
  }
}

void SimpleBluetoothActivity::handleKeyLearningInput() {
  if (btMgr && btMgr->isEnabled()) {
    uint8_t code = btMgr->consumeLastKeycode();
    if (code != 0x00) {
      learntKeycode = code;
      keyActionSelectedIndex = 0;
      state = BtPageState::KEY_ACTION_SELECT;
      updateRequired = true;
      return;
    }
  }
}

void SimpleBluetoothActivity::handleKeyActionInput() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    keyActionSelectedIndex = (keyActionSelectedIndex > 0) ? keyActionSelectedIndex - 1 : ACTION_COUNT - 1;
    updateRequired = true;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    keyActionSelectedIndex = (keyActionSelectedIndex < ACTION_COUNT - 1) ? keyActionSelectedIndex + 1 : 0;
    updateRequired = true;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    uint8_t btnAction = ACTION_TO_BTN[keyActionSelectedIndex];
    if (btnAction == 0xFF) {
      setBtKeyMapping(keyMappingSelectedSlot, 0x00, 0xFF);
    } else {
      setBtKeyMapping(keyMappingSelectedSlot, learntKeycode, btnAction);
    }
    SETTINGS.saveToFile();
    state = BtPageState::KEY_MAPPING;
    updateRequired = true;
  }
}

void SimpleBluetoothActivity::renderKeyMapping() {
  const char* BTN_NAMES[] = {L(Str::kBtnBack), L(Str::kBtnConfirm), L(Str::kBtnLeftArrow), L(Str::kBtnRightArrow), L(Str::kBtnUpArrow), L(Str::kBtnDownArrow)};
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, 15, L(Str::kBTKeyMappingTitle), true, EpdFontFamily::BOLD);
  
  constexpr int startY = 55;
  constexpr int lineHeight = 38;
  
  for (int i = 0; i < 6; i++) {
    const int y = startY + i * lineHeight;
    if (i == keyMappingSelectedSlot) {
      M4UiText::draw(renderer, UI_10_FONT_ID, 5, y, ">");
    }
    uint8_t code = getBtKeyCode(i);
    uint8_t action = getBtKeyAction(i);
    char buf[64];
    if (code != 0x00) {
      const char* actionName = (action < 6) ? BTN_NAMES[action] : L(Str::kUnmapped);
      snprintf(buf, sizeof(buf), L(Str::kKeyMapped), i + 1, code, actionName);
    } else {
      snprintf(buf, sizeof(buf), L(Str::kKeyNotSet), i + 1);
    }
    M4UiText::draw(renderer, UI_10_FONT_ID, 25, y, buf);
  }
  
  const auto labels = mappedInput.mapLabels(L(Str::kBtnBack), L(Str::kSet), "\u2191", "\u2193");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void SimpleBluetoothActivity::renderKeyLearning() {
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, 15, L(Str::kLearnKey), true, EpdFontFamily::BOLD);
  
  char slotBuf[32];
  snprintf(slotBuf, sizeof(slotBuf), L(Str::kConfigKey), keyMappingSelectedSlot + 1);
  M4UiText::drawCentered(renderer, UI_10_FONT_ID, 80, slotBuf);
  
  if (!btMgr || !btMgr->isEnabled()) {
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 160, L(Str::kBTNotEnabled));
  } else {
    auto connDevices = btMgr->getConnectedDevices();
    if (connDevices.empty()) {
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, 150, L(Str::kBTNotConnected));
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, 190, L(Str::kPleaseConnectFirst));
    } else {
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, 140, L(Str::kPressBTKey));
      M4UiText::drawCentered(renderer, UI_10_FONT_ID, 185, L(Str::kConnectedCanLearn));
    }
  }
  
  const auto labels = mappedInput.mapLabels(L(Str::kCancel), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, "", "", "", true);
}

void SimpleBluetoothActivity::renderKeyActionSelect() {
  const char* ACTION_LABELS[] = {
    L(Str::kBtnPrevPage), L(Str::kBtnNextPage), L(Str::kBtnLeft), L(Str::kBtnRight), L(Str::kBtnConfirm), L(Str::kBtnBack), L(Str::kClearMapping)
  };
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, 15, L(Str::kSelectFunction), true, EpdFontFamily::BOLD);
  
  char capturedBuf[48];
  snprintf(capturedBuf, sizeof(capturedBuf), L(Str::kKeyCaptured), keyMappingSelectedSlot + 1, learntKeycode);
  M4UiText::draw(renderer, UI_10_FONT_ID, 20, 45, capturedBuf);
  M4UiText::draw(renderer, UI_10_FONT_ID, 20, 68, L(Str::kMapTo));
  
  constexpr int startY = 95;
  constexpr int lineHeight = 38;
  
  for (int i = 0; i < ACTION_COUNT; i++) {
    const int y = startY + i * lineHeight;
    if (i == keyActionSelectedIndex) {
      M4UiText::draw(renderer, UI_10_FONT_ID, 5, y, ">");
    }
    M4UiText::draw(renderer, UI_10_FONT_ID, 25, y, ACTION_LABELS[i]);
  }
  
  const auto labels = mappedInput.mapLabels(L(Str::kCancel), L(Str::kBtnConfirm), "\u2191", "\u2193");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// ============ 连接成功/失败 ============

void SimpleBluetoothActivity::handleConnectedInput() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    // 完成按钮 - 退出蓝牙设置
    onComplete();
    return;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    // 返回按钮 - 回到设备列表
    state = BtPageState::DEVICE_LIST;
    updateRequired = true;
    return;
  }
}

void SimpleBluetoothActivity::handleConnectionFailedInput() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
      mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    // 任意键返回设备列表
    state = BtPageState::DEVICE_LIST;
    updateRequired = true;
    return;
  }
}
