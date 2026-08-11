#include "BluetoothHIDManager.h"
#include <NimBLEDevice.h>
#include <HalGPIO.h>
#include <WiFi.h>
#include <SDCardManager.h>
#include <Serialization.h>
#include <utility>
#include "../../src/CrossPointSettings.h"

// HID Service and characteristic UUIDs
static const char* HID_SERVICE_UUID = "1812";
static const char* HID_REPORT_UUID = "2A4D";
static const char* HID_INFO_UUID = "2A4A";

// Global static for singleton
static BluetoothHIDManager* g_instance = nullptr;

// Scan callbacks for NimBLE 2.x - keep as static to ensure it stays alive
class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
    Serial.printf("[%lu] [BT] onResult callback triggered!\n", millis());
    if (g_instance) {
      // onScanResult expects non-const pointer, need to cast
      g_instance->onScanResult(const_cast<NimBLEAdvertisedDevice*>(advertisedDevice));
    } else {
      Serial.printf("[%lu] [BT] onResult called but g_instance is NULL!\n", millis());
    }
  }
  
  void onScanEnd(const NimBLEScanResults& results, int reason) override {
    Serial.printf("[%lu] [BT] onScanEnd callback: %d devices, reason: %d\n", millis(), results.getCount(), reason);
  }
};

// Static instance to keep callbacks alive during scan
static ScanCallbacks scanCallbacks;

// Client connection callbacks
class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* pClient) override {
    Serial.printf("[%lu] [BT] Client connected: %s\n", millis(), pClient->getPeerAddress().toString().c_str());
  }
  
  void onDisconnect(NimBLEClient* pClient, int reason) override {
    Serial.printf("[%lu] [BT] Client disconnected: %s (reason: %d)\n", millis(), pClient->getPeerAddress().toString().c_str(), reason);
    Serial.printf("[%lu] [BT] Client %p kept alive for reuse (setSelfDelete=false)\n", millis(), pClient);
    
    // 关键修复：不再将client指针置空！
    // 保留client指针，这样：
    // 1. checkAutoReconnect() 能通过 device.client 检测到断开（isConnected()==false）
    // 2. connectToDevice() 的清理逻辑能正确 deleteClient 后再创建新的
    // 3. getDisconnectedClient() 能复用这个client对象
    // 之前置空会导致自动重连失效，且client对象泄漏
    if (g_instance) {
      for (auto& dev : g_instance->_connectedDevices) {
        if (dev.client == pClient) {
          Serial.printf("[%lu] [BT] Marking device %s for cleanup (wasConnected=true)\n", millis(), dev.address.c_str());
          dev.wasConnected = true;
          g_instance->_disconnectNotify = true;  // 通知主线程显示断开提示
          break;
        }
      }
    }
  }
};

BluetoothHIDManager& BluetoothHIDManager::getInstance() {
  if (!g_instance) {
    g_instance = new BluetoothHIDManager();
    Serial.printf("[%lu] [BT] BluetoothHIDManager instance created\n", millis());
  }
  return *g_instance;
}

BluetoothHIDManager::BluetoothHIDManager() {
  Serial.printf("[%lu] [BT] BluetoothHIDManager constructor\n", millis());
}

BluetoothHIDManager::~BluetoothHIDManager() {
  cleanup();
}

void BluetoothHIDManager::cleanup() {
  if (_enabled) {
    disable();
  }
}

bool BluetoothHIDManager::enable() {
  if (_enabled) {
    Serial.printf("[%lu] [BT] Already enabled\n", millis());
    return true;
  }
  
  Serial.printf("[%lu] [BT] Enabling Bluetooth...\n", millis());
  
  // CRITICAL: Disable WiFi when enabling Bluetooth
  // ESP32-C3 cannot have both WiFi and BLE enabled simultaneously
  if (WiFi.getMode() != WIFI_OFF) {
    Serial.printf("[%lu] [BT] Disabling WiFi to enable Bluetooth (mutual exclusion)\n", millis());
    WiFi.disconnect(true);  // true = turn off WiFi radio
    WiFi.mode(WIFI_OFF);
    delay(100);  // Brief delay to ensure WiFi is fully powered down
  }
  
  try {
    // 重新初始化NimBLE栈
    Serial.printf("[%lu] [BT] Initializing NimBLE stack...\n", millis());
    
    // Initialize NimBLE stack
    Serial.printf("[%lu] [BT] Initializing NimBLE...\n", millis());
    NimBLEDevice::init("CrossPoint");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9); // +9dBm
    NimBLEDevice::setSecurityAuth(true, false, true);
    
    _enabled = true;
    lastError = "";
    
    Serial.printf("[%lu] [BT] Bluetooth enabled successfully\n", millis());
    loadState();
    
    // NOTE: background auto‑connect was removed in order to conserve memory
    // and avoid multiple concurrent connection attempts.  callers such as
    // main.cpp or the settings UI should invoke connectToDeviceWithRetries()
    // explicitly when appropriate.
    
    return true;
  } catch (const std::exception& e) {
    Serial.printf("[%lu] [BT] Failed to enable Bluetooth: %s\n", millis(), e.what());
    lastError = std::string("Init failed: ") + e.what();
    _enabled = false;
    return false;
  } catch (...) {
    Serial.printf("[%lu] [BT] Failed to enable Bluetooth: unknown error\n", millis());
    lastError = "Init failed: unknown error";
    _enabled = false;
    return false;
  }
}

bool BluetoothHIDManager::disable() {
  if (!_enabled) {
    Serial.printf("[%lu] [BT] Already disabled\n", millis());
    return true;
  }
  
  Serial.printf("[%lu] [BT] Disabling Bluetooth... (connected devices: %zu)\n", millis(), _connectedDevices.size());
  
  // 先标记为禁用，阻止 checkAutoReconnect / connectToDevice 等后续操作
  _enabled = false;
  
  Serial.printf("[%lu] [BT] Disabling Bluetooth...\n", millis());
  
  if (_scanning) {
    stopScan();
  }
  
  // 清理设备列表，置空 client 指针防止 deinit 后被访问
  Serial.printf("[%lu] [BT] Clearing connected devices...\n", millis());
  {
    DeviceListGuard guard(_modifyingDevices);
    for (auto& dev : _connectedDevices) {
      dev.client = nullptr;  // 防止 updateActivity 在 deinit 后访问已释放的 client
    }
    _connectedDevices.clear();
  }
  
  // 同步 deinit
  Serial.printf("[%lu] [BT] Deiniting NimBLE stack...\n", millis());
  try {
    NimBLEDevice::deinit(false);
  } catch (...) {
    Serial.printf("[%lu] [BT] Warning: error during deinit\n", millis());
  }
  
  lastError = "";
  _disconnectNotify = false;  // 清除断开通知，避免 disable 后弹窗
  
  Serial.printf("[%lu] [BT] Bluetooth disabled\n", millis());
  return true;
}

void BluetoothHIDManager::startScan(uint32_t durationMs) {
  if (!_enabled || _scanning) {
    Serial.printf("[%lu] [BT] Cannot scan: enabled=%d scanning=%d\n", millis(), _enabled, _scanning);
    return;
  }
  
  Serial.printf("[%lu] [BT] Starting BLE scan for %lu ms\n", millis(), durationMs);
  
  // 清理已断开的设备
  {
    DeviceListGuard guard(_modifyingDevices);
    auto it = _connectedDevices.begin();
    while (it != _connectedDevices.end()) {
      if (!it->client || !it->client->isConnected()) {
        Serial.printf("[%lu] [BT] Cleanup disconnected device: %s\n", millis(), it->address.c_str());
        if (it->client) {
          try {
            NimBLEDevice::deleteClient(it->client);
          } catch (...) {
            // Ignore
          }
        }
        it = _connectedDevices.erase(it);
      } else {
        ++it;
      }
    }
  }
  
  _scanning = true;
  _discoveredDevices.clear();
  _scanStartTime = millis();
  _scanDuration = durationMs;
  
  try {
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (!pScan) {
      Serial.printf("[%lu] [BT] Failed to get scan object\n", millis());
      _scanning = false;
      return;
    }
    
    Serial.printf("[%lu] [BT] Setting up scan callbacks...\n", millis());
    pScan->setScanCallbacks(&scanCallbacks, false);
    pScan->setActiveScan(true);  // 主动扫描，会发送SCAN_REQUEST
    pScan->setInterval(100);     // 扫描间隔 100ms
    pScan->setWindow(99);        // 扫描窗口 99ms（几乎持续扫描）
    
    Serial.printf("[%lu] [BT] Starting continuous scan (non-blocking)...\n", millis());
    // 关键：duration=0 表示持续扫描，不阻塞
    bool started = pScan->start(0, false);
    
    if (!started) {
      Serial.printf("[%lu] [BT] Failed to start scan!\n", millis());
      _scanning = false;
      return;
    }
    
    Serial.printf("[%lu] [BT] Scan started successfully\n", millis());
    
  } catch (const std::exception& e) {
    Serial.printf("[%lu] [BT] Scan failed: %s\n", millis(), e.what());
    _scanning = false;
    lastError = std::string("Scan failed: ") + e.what();
  }
}

void BluetoothHIDManager::stopScan() {
  if (!_scanning) return;
  
  Serial.printf("[%lu] [BT] Stopping scan\n", millis());
  
  try {
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan) {
      pScan->stop();
      pScan->clearResults();  // 释放 NimBLE 内部扫描结果缓存
    }
  } catch (...) {
    Serial.printf("[%lu] [BT] Error stopping scan\n", millis());
  }
  
  _scanning = false;
  _scanDuration = 0;
  size_t deviceCount = _discoveredDevices.size();
  Serial.printf("[%lu] [BT] Scan stopped, found %zu devices\n", millis(), deviceCount);
  
  // 注意：不在这里清空设备列表！
  // 设备列表应该在开始新扫描时清空，这样UI才有时间显示结果
  // _discoveredDevices.clear();  // 删除这行
}

void BluetoothHIDManager::onScanResult(NimBLEAdvertisedDevice* advertisedDevice) {
  if (!advertisedDevice) return;
  
  std::string address = advertisedDevice->getAddress().toString();
  int rssi = advertisedDevice->getRSSI();
  std::string name = advertisedDevice->getName();
  
  // 检查是否为 HID 设备 - 使用多种检测方式
  // 策略：宽松扫描，严格连接。扫描阶段尽量多展示设备，
  // 连接时再验证是否有 HID 服务（0x1812），这和手机的行为一致。
  bool isHID = false;
  bool isConfirmedHID = false;  // 确认的HID设备（高置信度）
  
  // 方法1：检查是否广播HID服务（高置信度）
  if (advertisedDevice->haveServiceUUID()) {
    if (advertisedDevice->isAdvertisingService(NimBLEUUID(HID_SERVICE_UUID))) {
      isHID = true;
      isConfirmedHID = true;
    }
  }
  
  // 方法2：检查Appearance字段 - 扩展识别范围
  if (!isHID && advertisedDevice->haveAppearance()) {
    uint16_t appearance = advertisedDevice->getAppearance();
    // HID相关Appearance值（Bluetooth SIG定义）
    // 0x03C0 = Generic HID, 0x03C1 = Keyboard, 0x03C2 = Mouse,
    // 0x03C3 = Joystick, 0x03C4 = Gamepad, 0x03C5 = Digitizer Tablet,
    // 0x03C6 = Card Reader, 0x03C7 = Digital Pen, 0x03C8 = Barcode Scanner
    // 0x0180 = Generic Remote Control
    if ((appearance >= 0x03C0 && appearance <= 0x03C8) || appearance == 0x0180) {
      isHID = true;
      isConfirmedHID = true;
    }
  }
  
  // 方法3：检查设备名称关键词（扩展关键词列表）
  if (!isHID && !name.empty()) {
    std::string lowerName = name;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
    // 翻页器/遥控器/键盘/鼠标常见关键词
    const char* hidKeywords[] = {
      "keyboard", "mouse", "iine", "page", "remote", "clicker",
      "presenter", "free2", "free3", "gamepad", "joystick",
      "controller", "hid", "input", "btn", "key", "flip",
      "turner", "翻页", "遥控", nullptr
    };
    for (int i = 0; hidKeywords[i] != nullptr; i++) {
      if (lowerName.find(hidKeywords[i]) != std::string::npos) {
        isHID = true;
        break;
      }
    }
  }
  
  // 方法4（新增）：有名称的设备都作为候选显示
  // 很多翻页器不广播HID UUID也不设Appearance，但有设备名称
  // 连接后再验证HID服务，这和手机的行为一致
  if (!isHID && !name.empty()) {
    isHID = true;
  }
  
  // 方法5：无名称设备也显示（RSSI > -80 的近距离设备）
  // 部分便宜翻页器初始广播不带名称，连接后才能获取
  if (!isHID && rssi > -80) {
    isHID = true;
  }
  
  // 调试：打印所有扫描到的设备（包括被过滤的）
  const std::string prefix = (address.size() >= 8) ? address.substr(0, 8) : address;
  Serial.printf("[%lu] [BT] Scan device: %s (%s) RSSI:%d HID:%d confirmed:%d\n", 
                millis(), name.empty() ? "Unknown" : name.c_str(), prefix.c_str(), rssi, isHID, isConfirmedHID);
  
  // 只保存 HID 设备
  if (!isHID) return;
  
  // Check if we already have this device
  for (auto& dev : _discoveredDevices) {
    if (dev.address == address) {
      dev.rssi = rssi; // Update RSSI
      return;
    }
  }
  
  // Add new device
  BluetoothDevice device;
  device.address = address;
  device.name = name.empty() ? "Unknown" : name;
  device.rssi = rssi;
  device.isHID = true;
  
  _discoveredDevices.push_back(device);
  
  Serial.printf("[%lu] [BT] Found HID device: %s (%s) RSSI:%d\n", 
                millis(), device.name.c_str(), device.address.c_str(), rssi);
}



bool BluetoothHIDManager::connectToDevice(const std::string& address) {
  if (!_enabled) {
    Serial.printf("[%lu] [BT] Cannot connect: Bluetooth not enabled\n", millis());
    lastError = "Bluetooth not enabled";
    return false;
  }
  if (address.empty()) {
    Serial.printf("[%lu] [BT] Invalid address (empty) passed to connectToDevice\n", millis());
    lastError = "Invalid address";
    return false;
  }
  
  // Check if already connected
  if (isConnected(address)) {
    Serial.printf("[%lu] [BT] Already connected to %s\n", millis(), address.c_str());
    return true;
  }
  
  // 宽松策略：不再要求设备必须在扫描结果中
  // 扫描结果中的设备可能因为过滤或时序问题不完整
  // 连接后会验证HID服务（0x1812），这才是真正的判断标准
  bool seen = false;
  for (const auto& dev : _discoveredDevices) {
    if (dev.address == address) {
      seen = true;
      break;
    }
  }
  if (!seen) {
    Serial.printf("[%lu] [BT] Device %s not in scan results, attempting connection anyway\n", millis(), address.c_str());
  }
  
  Serial.printf("[%lu] [BT] Connecting to device %s\n", millis(), address.c_str());
  
  // 清理已断开的设备
  {
    DeviceListGuard guard(_modifyingDevices);
    auto it = _connectedDevices.begin();
    while (it != _connectedDevices.end()) {
      if (!it->client || !it->client->isConnected()) {
        Serial.printf("[%lu] [BT] Cleanup disconnected device before connect: %s\n", millis(), it->address.c_str());
        if (it->client) {
          try {
            NimBLEDevice::deleteClient(it->client);
          } catch (...) {
            // Ignore
          }
        }
        it = _connectedDevices.erase(it);
      } else {
        ++it;
      }
    }
  }
  
  NimBLEClient* pClient = nullptr;
  
  // 终极方案：完全不删除client，永久复用！
  // setSelfDelete(true/false)都会导致崩溃，唯一的办法就是永不删除
  
  try {
    // 先尝试获取已断开的client复用
    pClient = NimBLEDevice::getDisconnectedClient();
    if (pClient) {
      Serial.printf("[%lu] [BT] Reusing disconnected client %p\n", millis(), pClient);
    } else {
      // 没有断开的client，创建新的
      pClient = NimBLEDevice::createClient();
      if (!pClient) {
        lastError = "Failed to create client";
        Serial.printf("[%lu] [BT] Failed to create BLE client - pool exhausted!\n", millis());
        return false;
      }
      Serial.printf("[%lu] [BT] Created new client %p\n", millis(), pClient);
    }
    
    // 禁用自动删除，由我们手动管理client生命周期
    // 避免NimBLE回调线程和主线程同时删除导致double-free
    pClient->setSelfDelete(false, false);
    
    // Set connection callbacks
    static ClientCallbacks clientCallbacks;
    pClient->setClientCallbacks(&clientCallbacks);
    
    // Connect to device
    NimBLEAddress bleAddress(address, BLE_ADDR_PUBLIC);
    if (!pClient->connect(bleAddress)) {
      lastError = "Connection failed";
      Serial.printf("[%lu] [BT] Failed to connect to %s\n", millis(), address.c_str());
      // 不deleteClient，让getDisconnectedClient()下次复用
      return false;
    }

    // 连接成功后更新连接参数：
    // - 连接间隔保持短（最大 100ms），GATT 请求应答快，订阅过程不卡顿
    // - 监督超时延长到 5s，信号弱时有充足容错时间再断开
    // 约束校验： timeout(5000ms) > (1+latency)*maxInterval*2 = (1+0)*100*2 = 200ms ✓
    pClient->updateConnParams(16, 80, 0, 500);  // min=20ms, max=100ms, latency=0, timeout=5s
    Serial.printf("[%lu] [BT] Connection params updated: interval 20-100ms, supervision 5s\n", millis());

    Serial.printf("[%lu] [BT] Connected, discovering services...\n", millis());
    
    // Get HID service
    NimBLERemoteService* pService = pClient->getService(HID_SERVICE_UUID);
    if (!pService) {
      lastError = "HID service not found";
      Serial.printf("[%lu] [BT] Device %s doesn't have HID service\n", millis(), address.c_str());
      pClient->disconnect();
      return false;
    }
    
    Serial.printf("[%lu] [BT] Found HID service, enumerating report characteristics...\n", millis());
    
    auto pCharacteristics = pService->getCharacteristics(true);
    NimBLERemoteCharacteristic* pReportChar = nullptr;
    
    int reportCount = 0;
    std::vector<NimBLERemoteCharacteristic*> reportChars;
    
    for (auto it = pCharacteristics.begin(); it != pCharacteristics.end(); ++it) {
      auto* pChar = *it;
      Serial.printf("[%lu] [BT] Characteristic UUID: %s, canRead:%d canWrite:%d canNotify:%d canIndicate:%d\n",
              millis(), pChar->getUUID().toString().c_str(),
              pChar->canRead(), pChar->canWrite(), pChar->canNotify(), pChar->canIndicate());
      
      if (pChar->getUUID().equals(NimBLEUUID(HID_REPORT_UUID))) {
        reportCount++;
        Serial.printf("[%lu] [BT] Found Report char #%d, notify:%d indicate:%d UUID:%s\n", 
                millis(), reportCount, pChar->canNotify(), pChar->canIndicate(),
                pChar->getUUID().toString().c_str());
        
        if (pChar->canNotify() || pChar->canIndicate()) {
          reportChars.push_back(pChar);
          Serial.printf("[%lu] [BT] Added Report char #%d for subscription\n", millis(), reportCount);
        }
      }
    }
    
    if (reportChars.empty()) {
      lastError = "No input report characteristic found";
      Serial.printf("[%lu] [BT] No Report characteristic with notify/indicate found\n", millis());
      pClient->disconnect();
      return false;
    }
    
    Serial.printf("[%lu] [BT] Subscribing to %zu Report characteristics...\n", millis(), reportChars.size());
    
    for (size_t i = 0; i < reportChars.size(); i++) {
      auto* pChar = reportChars[i];
      Serial.printf("[%lu] [BT] Subscribing to Report char #%zu...\n", millis(), i + 1);
      bool subResult = pChar->subscribe(true, onHIDNotify);
      Serial.printf("[%lu] [BT] Report char #%zu subscribe result: %d\n", millis(), i + 1, subResult);
      if (!subResult) {
        Serial.printf("[%lu] [BT] Failed to subscribe to Report char #%zu (continuing)\n", millis(), i + 1);
      }
    }
    
    ConnectedDevice connDev;
    connDev.address = address;
    connDev.client = pClient;
    connDev.reportChars = reportChars;
    connDev.subscribed = true;
    connDev.lastActivityTime = millis();
    connDev.wasConnected = true;
    
    bool foundInScan = false;
    for (const auto& dev : _discoveredDevices) {
      if (dev.address == address) {
        connDev.name = dev.name;
        foundInScan = true;
        Serial.printf("[%lu] [BT] Device found in scan results: %s (%s)\n", millis(), dev.name.c_str(), address.c_str());
        break;
      }
    }
    
    if (!foundInScan) {
      Serial.printf("[%lu] [BT] Device not in scan results (may be previously paired): %s\n", millis(), address.c_str());
    }
    
    connDev.profile = DeviceProfiles::findDeviceProfile(address.c_str(), connDev.name.c_str());
    
    if (connDev.profile) {
      Serial.printf("[%lu] [BT] ✓ Using device profile: %s (byte[%d] for keycode)\n", 
              millis(), connDev.profile->name, connDev.profile->reportByteIndex);
    } else {
      Serial.printf("[%lu] [BT] No known profile matched for %s, will auto-detect from HID codes\n", millis(), address.c_str());
    }
    
    {
      DeviceListGuard guard(_modifyingDevices);
      _connectedDevices.push_back(connDev);
    }
    
    Serial.printf("[%lu] [BT] Successfully connected to %s\n", millis(), address.c_str());
    lastError = "Connected";
    
    saveLastConnectedDevice(address, connDev.name);
    
    return true;
    
  } catch (const std::exception& e) {
    lastError = std::string("Connection error: ") + e.what();
    Serial.printf("[%lu] [BT] %s\n", millis(), lastError.c_str());
    if (pClient) {
      try {
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
      } catch (...) {
        Serial.printf("[%lu] [BT] Warning: failed to cleanup client after exception\n", millis());
      }
    }
    return false;
  } catch (...) {
    lastError = "Unknown connection error";
    Serial.printf("[%lu] [BT] %s\n", millis(), lastError.c_str());
    if (pClient) {
      try {
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
      } catch (...) {
        Serial.printf("[%lu] [BT] Warning: failed to cleanup client after unknown exception\n", millis());
      }
    }
    return false;
  }
}




// Simple retry wrapper around connectToDevice.  It invokes the single-
// attempt function up to `maxAttempts` times with a short delay between
// tries.  This keeps higher-level code (UI) from having to loop itself.
bool BluetoothHIDManager::connectToDeviceWithRetries(const std::string& address, int maxAttempts) {
  if (!_enabled) {
    Serial.printf("[%lu] [BT] Cannot connect (retries): Bluetooth not enabled\n", millis());
    lastError = "Bluetooth not enabled";
    return false;
  }
  if (address.empty()) {
    Serial.printf("[%lu] [BT] Invalid address (empty) passed to connectToDeviceWithRetries\n", millis());
    lastError = "Invalid address";
    return false;
  }
  if (isConnected(address)) {
    Serial.printf("[%lu] [BT] Already connected to %s (retries)\n", millis(), address.c_str());
    return true;
  }
  if (maxAttempts <= 0) maxAttempts = 1;
  for (int i = 0; i < maxAttempts; ++i) {
    Serial.printf("[%lu] [BT] retry %d/%d for %s\n", millis(), i + 1, maxAttempts, address.c_str());
    if (connectToDevice(address)) {
      return true;
    }
    delay(200);
  }
  return false;
}

bool BluetoothHIDManager::disconnectFromDevice(const std::string& address) {
  Serial.printf("[%lu] [BT] Disconnecting from device %s\n", millis(), address.c_str());
  
  auto it = std::find_if(_connectedDevices.begin(), _connectedDevices.end(),
    [&address](const ConnectedDevice& dev) { return dev.address == address; });
  
  if (it != _connectedDevices.end()) {
    NimBLEClient* pClient = it->client;
    
    // 断开连接，但不删除client（setSelfDelete=false）
    // client会保留在NimBLE内部，下次getDisconnectedClient()复用
    // 这避免了NimBLE回调线程和主线程的竞争条件
    if (pClient && pClient->isConnected()) {
      try {
        pClient->disconnect();
        delay(100);
      } catch (...) {
        // Ignore
      }
    }
    
    // 从列表中移除，但不deleteClient
    { DeviceListGuard guard(_modifyingDevices); _connectedDevices.erase(it); }
    
    Serial.printf("[%lu] [BT] Disconnected from %s\n", millis(), address.c_str());
    return true;
  }
  
  Serial.printf("[%lu] [BT] Device %s not in connected list\n", millis(), address.c_str());
  return false;
}

bool BluetoothHIDManager::isConnected(const std::string& address) const {
  return std::find_if(_connectedDevices.begin(), _connectedDevices.end(),
    [&address](const ConnectedDevice& dev) { return dev.address == address; }) != _connectedDevices.end();
}

std::vector<std::string> BluetoothHIDManager::getConnectedDevices() const {
  std::vector<std::string> addresses;
  for (const auto& dev : _connectedDevices) {
    addresses.push_back(dev.address);
  }
  return addresses;
}

ConnectedDevice* BluetoothHIDManager::findConnectedDevice(const std::string& address) {
  auto it = std::find_if(_connectedDevices.begin(), _connectedDevices.end(),
    [&address](const ConnectedDevice& dev) { return dev.address == address; });
  
  if (it != _connectedDevices.end()) {
    return &(*it);
  }
  return nullptr;
}

void BluetoothHIDManager::processInputEvents() {
  // Input events are processed via notifications callback
  // This method is kept for potential polling-based implementations
}

void BluetoothHIDManager::setInputCallback(std::function<void(uint16_t)> callback) {
  _inputCallback = callback;
  Serial.printf("[%lu] [BT] Input callback registered\n", millis());
}

void BluetoothHIDManager::setButtonInjector(std::function<void(uint8_t)> injector) {
  _buttonInjector = injector;
  Serial.printf("[%lu] [BT] Button injector registered\n", millis());
}

bool BluetoothHIDManager::hasRecentActivity() const {
  // Check if any connected device has had activity in the last 4 minutes
  // This prevents power sleep while using BLE controller
  unsigned long now = millis();
  for (const auto& device : _connectedDevices) {
    if (device.lastActivityTime > 0) {
      unsigned long timeSinceActivity = now - device.lastActivityTime;
      if (timeSinceActivity < 240000) {  // 4 minute (240 second) threshold to keep BLE alive
        return true;
      }
    }
  }
  return false;
}

// Static callback for HID notifications
void BluetoothHIDManager::onHIDNotify(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
  if (!g_instance || !pData || length == 0) return;
  
  // 安全检查：确保pChar有效
  if (!pChar) return;
  
  // 线程安全：如果主线程正在修改设备列表，跳过本次回调
  // 避免 use-after-free 崩溃（ESP32-C3 单核，但 NimBLE 用 FreeRTOS 任务）
  if (g_instance->_modifyingDevices) return;
  
  // Skip all-zero (idle/keepalive) reports to avoid log flooding
  bool allZero = true;
  for (size_t i = 0; i < length; i++) {
    if (pData[i] != 0x00) { allZero = false; break; }
  }
  
  // 遍历设备列表，找到匹配的client
  ConnectedDevice* device = nullptr;
  for (auto& dev : g_instance->_connectedDevices) {
    if (!dev.client) continue;
    
    try {
      auto* pService = pChar->getRemoteService();
      if (pService && pService->getClient() == dev.client) {
        if (dev.client->isConnected()) {
          device = &dev;
          break;
        }
      }
    } catch (...) {
      // Ignore
    }
  }
  
  if (!device) return;
  
  // 更新活动时间戳
  device->lastActivityTime = millis();
  
  if (allZero) {
    // All-zero = "no button pressed" (release event)
    // Reset button state so next press triggers a new transition
    device->lastButtonState = false;
    device->lastHIDKeycode = 0x00;
    return;
  }

  unsigned long now = millis();

  // ========== 【第一重去重】提前过滤：在解析和打印日志之前，丢弃500ms内的重复原始报告 ==========
  // 提取原始keycode用于快速去重（byte[2]是标准HID键盘格式的keycode位置）
  // 这样重复报告不会产生任何日志，与4月19日版本行为一致
  uint8_t rawKeycode = (length >= 3) ? pData[2] : (length >= 1 ? pData[0] : 0x00);
  static uint8_t lastRawKeycode = 0x00;
  static unsigned long lastRawKeycodeTime = 0;
  static bool buttonInjected = false;

  if (rawKeycode != 0x00 && rawKeycode == lastRawKeycode && (now - lastRawKeycodeTime) < 500) {
    return;  // 静默忽略：相同按键在500ms内的重复报告，不打印任何日志
  }
  if (rawKeycode != 0x00) {
    lastRawKeycode = rawKeycode;
    lastRawKeycodeTime = now;
    buttonInjected = false;  // 新按键周期开始，允许一次注入
  }

  // ========== 通过第一重去重，现在才打印日志 ==========
  char hexStr[128] = {0};
  int offset = 0;
  for (size_t i = 0; i < length && i < 16; i++) {
    offset += snprintf(hexStr + offset, sizeof(hexStr) - offset, "%02X ", pData[i]);
  }
  Serial.printf("[%lu] [BT] HID Report (%zu bytes): %s\n", millis(), length, hexStr);

  if (length < 1) {
    Serial.printf("[%lu] [BT] HID report too short (%zu bytes)\n", millis(), length);
    return;
  }

  // Extract keycode and press state based on device profile or auto-detect
  uint8_t keycode = 0xFF;
  bool isPressed = false;

  if (device->profile) {
    if (length >= device->profile->reportByteIndex + 1) {
      keycode = pData[device->profile->reportByteIndex];
    }
    if (strcmp(device->profile->name, "IINE Game Brick") == 0) {
      isPressed = (pData[0] & 0x01) != 0;
      if (!isPressed) { device->lastButtonState = false; device->lastHIDKeycode = 0x00; return; }
      Serial.printf("[%lu] [BT] Game Brick: byte[0]=0x%02X, keycode=0x%02X, pressed=%d\n", millis(), pData[0], keycode, isPressed);
    } else {
      isPressed = (keycode != 0x00);
      if (!isPressed) { device->lastButtonState = false; device->lastHIDKeycode = 0x00; return; }
      Serial.printf("[%lu] [BT] Device %s: keycode=0x%02X, pressed=%d\n", millis(), device->profile->name, keycode, isPressed);
    }
  } else {
    // Auto-detect mode
    if (length == 1) {
      keycode = pData[0];
      isPressed = (keycode != 0x00);
      Serial.printf("[%lu] [BT] Auto-detect (1-byte): keycode=0x%02X\n", millis(), keycode);
    } else if (length == 2) {
      keycode = pData[0];
      isPressed = (keycode != 0x00);
      Serial.printf("[%lu] [BT] Auto-detect (Consumer Control 2-byte): keycode=0x%02X 0x%02X\n", millis(), pData[0], pData[1]);
    } else if (length == 3) {
      uint8_t modifier = pData[0];
      keycode = pData[2];
      if (keycode == 0x00 && modifier != 0x00) {
        if (modifier == 0x01)      keycode = 0x4B;  // PageUp
        else if (modifier == 0x02) keycode = 0x4E;  // PageDown
        else                       keycode = modifier;
      }
      isPressed = (keycode != 0x00);
      Serial.printf("[%lu] [BT] Auto-detect (3-byte): 修饰位=0x%02X, 按键码=0x%02X, pressed=%d\n", millis(), modifier, keycode, isPressed);
    } else if (length >= 5) {
      uint8_t byte4 = pData[4];
      if (byte4 == 0x07 || byte4 == 0x09) {
        keycode = byte4;
        isPressed = (pData[0] & 0x01) != 0;
        if (!isPressed) { device->lastButtonState = false; device->lastHIDKeycode = 0x00; return; }
        Serial.printf("[%lu] [BT] Auto-detect (GameBrick): byte[4]=0x%02X\n", millis(), keycode);
      } else {
        keycode = pData[2];
        isPressed = (keycode != 0x00);
        Serial.printf("[%lu] [BT] Auto-detect (standard %zu-byte): keycode=0x%02X\n", millis(), length, keycode);
      }
    } else {
      keycode = pData[2];
      isPressed = (keycode != 0x00);
      Serial.printf("[%lu] [BT] Auto-detect (standard %zu-byte): keycode=0x%02X\n", millis(), length, keycode);
    }
  }

  if (keycode == 0x00 || keycode == 0xFF) {
    device->lastButtonState = false;
    device->lastHIDKeycode = 0x00;
    return;
  }

  // Track raw keycode for the key learning UI
  g_instance->_lastRawKeycode = keycode;

  // ========== 【第二重去重】按键注入 ==========
  // 兼容两种 BT 设备行为：
  //   A) 有显式 0x00 Release 的设备：每次 press 前 lastButtonState 已被 0x00 重置为 false（边沿触发）
  //   B) 无 0x00 Release 的设备（本设备）：press 报文连续重复，lastButtonState 始终为 true
  // 统一策略：isPressed 时均允许注入，由第一重去重（500ms 窗口 + buttonInjected 标志）控制频率。
  if (isPressed) {
    // ------ Hold-sequence tracking ------
    // 统一在每次 press 事件（无论边沿还是重复）更新序列计时，
    // 支持无 0x00 的设备（lastButtonState 始终 true，若仅在边沿更新则序列永远不累积）。
    constexpr unsigned long BT_SEQ_GAP_RESET_MS = 1000UL;
    if (device->lastPressEventMs == 0 ||
        (now - device->lastPressEventMs) > BT_SEQ_GAP_RESET_MS ||
        (device->lastHIDKeycode != 0x00 && keycode != device->lastHIDKeycode)) {
      // 新的按住序列：超时或键码变化时重置
      device->holdSequenceStartMs = now;
    }
    device->lastPressEventMs = now;
    device->lastHIDKeycode = keycode;  // 记录最后按键的键码，用于下次预测是否切换了按键
    g_instance->_btHoldDurationMs = now - device->holdSequenceStartMs;
    // ------------------------------------

    const char* pressType = device->lastButtonState ? "AUTO-REPEAT" : "EDGE";
    Serial.printf("[%lu] [BT] >>> BUTTON %s: keycode=0x%02X (holdMs=%lu) <<<\n",
                  millis(), pressType, keycode, g_instance->_btHoldDurationMs);

    if (!buttonInjected && g_instance->_buttonInjector) {
      uint8_t btn = g_instance->mapKeycodeToButton(keycode, device->profile);
      if (btn != 0xFF) {
        const char* btnNames[] = { "Back/返回", "Confirm/确认", "Left/左", "Right/右", "Up/上一页", "Down/下一页", "Power" };
        const char* btnName = (btn < 7) ? btnNames[btn] : "Unknown";
        Serial.printf("[%lu] [BT] Mapped key 0x%02X -> %s (btn=%d)\n", millis(), keycode, btnName, btn);
        Serial.printf("[%lu] [BT] Injecting button: %d\n", millis(), btn);
        g_instance->_buttonInjector(btn);
        buttonInjected = true;  // 本次 500ms 窗口内只注入一次
      }
    }

    if (g_instance->_inputCallback) {
      g_instance->_inputCallback(keycode);
    }
  }

  device->lastButtonState = isPressed;
  device->lastHIDKeycode = keycode;
}

uint16_t BluetoothHIDManager::parseHIDReport(uint8_t* data, size_t length) {
  if (length < 3) {
    Serial.printf("[%lu] [BT] Invalid HID report length: %zu\n", millis(), length);
    return 0;
  }
  
  uint8_t modifier = data[0];
  uint8_t keycode = data[2]; // First key in the report
  
  // If no key pressed (all zeros), return 0
  if (keycode == 0 && modifier == 0) {
    return 0;
  }
  
  // Log non-empty reports
  Serial.printf("[%lu] [BT] HID Report: mod=0x%02X key=0x%02X\n", millis(), modifier, keycode);
  
  // Combine modifier and keycode (modifier in upper byte, keycode in lower)
  uint16_t combined = (static_cast<uint16_t>(modifier) << 8) | keycode;
  
  return combined;
}

uint8_t BluetoothHIDManager::consumeLastKeycode() {
  uint8_t code = _lastRawKeycode;
  _lastRawKeycode = 0x00;
  return code;
}

// Map HID keycodes to navigator buttons based on device profile
// Only maps keycodes that match the current device's profile to prevent
// unwanted D-pad or other button inputs from triggering page turns
uint8_t BluetoothHIDManager::mapKeycodeToButton(uint8_t keycode, const DeviceProfiles::DeviceProfile* profile) {
  // Log keycode for debugging
  if (keycode != 0x00) {
    Serial.printf("[%lu] [BT] mapKeycodeToButton() called with keycode: 0x%02X\n", millis(), keycode);
  }
  
  // Priority 0: Check user-configured BT key mappings (stored in CrossPointSettings)
  // This allows full customization of any 6 BT keys to any device button
  const struct { uint8_t code; uint8_t action; } userMappings[6] = {
    { SETTINGS.btKey1Code, SETTINGS.btKey1Action },
    { SETTINGS.btKey2Code, SETTINGS.btKey2Action },
    { SETTINGS.btKey3Code, SETTINGS.btKey3Action },
    { SETTINGS.btKey4Code, SETTINGS.btKey4Action },
    { SETTINGS.btKey5Code, SETTINGS.btKey5Action },
    { SETTINGS.btKey6Code, SETTINGS.btKey6Action },
  };
  for (int i = 0; i < 6; i++) {
    if (userMappings[i].code != 0x00 && userMappings[i].code == keycode && userMappings[i].action != 0xFF) {
      Serial.printf("[%lu] [BT] User mapping: 0x%02X -> BTN %d\n", millis(), keycode, userMappings[i].action);
      return userMappings[i].action;
    }
  }
  
  // If we have a device profile, ONLY map keycodes specific to that profile
  if (profile) {
    if (keycode == profile->pageUpCode) {
      Serial.printf("[%lu] [BT] Matched profile pageUpCode 0x%02X (%s) -> PageBack\n", millis(), keycode, profile->name);
      return HalGPIO::BTN_UP;
    } else if (keycode == profile->pageDownCode) {
      Serial.printf("[%lu] [BT] Matched profile pageDownCode 0x%02X (%s) -> PageForward\n", millis(), keycode, profile->name);
      return HalGPIO::BTN_DOWN;
    } else {
      // Not a profile-mapped keycode - ignore it
      Serial.printf("[%lu] [BT] Keycode 0x%02X not in profile %s (expecting 0x%02X/0x%02X), ignoring\n", 
              millis(), keycode, profile->name, profile->pageUpCode, profile->pageDownCode);
      return 0xFF;
    }
  }
  
  // No profile - fall back to generic HID consumer codes only
  switch (keycode) {
    // 翻页器核心映射（按需修改）
    case 0x01:   // 左Ctrl → 上一页
    case 0x4B:   // PageUp → 上一页
    case 0xE9:   // Consumer Volume Up / PageUp → 上一页
    case 0xB6:   // Consumer Scan Previous Track → 上一页
    case 0x29:   // Keyboard Escape → 上一页（部分翻页器）
      Serial.printf("[%lu] [BT] Mapped key 0x%02X -> PageBack\n", millis(), keycode);
      return HalGPIO::BTN_UP;
    
    case 0x02:   // 左Shift → 下一页
    case 0x4E:   // PageDown → 下一页
    case 0xEA:   // Consumer Volume Down / PageDown → 下一页
    case 0xB5:   // Consumer Scan Next Track → 下一页
    case 0x28:   // Keyboard Return/Enter → 下一页（部分翻页器）
    case 0x2C:   // Keyboard Space → 下一页（部分翻页器）
      Serial.printf("[%lu] [BT] Mapped key 0x%02X -> PageForward\n", millis(), keycode);
      return HalGPIO::BTN_DOWN;

    case 0x00:   // 忽略释放事件
      return 0xFF;

    default:
      Serial.printf("[%lu] [BT] Unmapped keycode: 0x%02X (翻页器未匹配)\n", millis(), keycode);
      return 0xFF;
  }
}

void BluetoothHIDManager::updateActivity() {
  if (!_enabled) return;  // deinit 后不访问任何 NimBLE 资源
  
  unsigned long now = millis();
  
  // ========== 清理已断开的设备 ==========
  // 定期检查并清理（但保留 wasConnected 的设备给 checkAutoReconnect 处理）
  static unsigned long lastCleanup = 0;
  if (_enabled && now - lastCleanup > 3000) {
    lastCleanup = now;
    
    // 清理_connectedDevices中已断开且不需要重连的设备
    DeviceListGuard guard(_modifyingDevices);
    auto it = _connectedDevices.begin();
    while (it != _connectedDevices.end()) {
      if (!it->client || !it->client->isConnected()) {
        if (it->wasConnected) {
          // 需要自动重连的设备，保留给 checkAutoReconnect() 处理
          ++it;
          continue;
        }
        Serial.printf("[%lu] [BT] Cleanup: removing disconnected device %s\n", millis(), it->address.c_str());
        it = _connectedDevices.erase(it);
      } else {
        ++it;
      }
    }
  }
  
  // ========== 检查扫描超时 ==========
  if (_scanning && _scanDuration > 0) {
    if (now - _scanStartTime >= _scanDuration) {
      Serial.printf("[%lu] [BT] Scan timeout after %lu ms\n", millis(), _scanDuration);
      stopScan();
    }
  }
  
  // 原有的不活跃检查（每10秒一次）
  if (now - lastMaintenanceCheck < 10000) {
    return;
  }
  lastMaintenanceCheck = now;
  
  if (!_enabled) return;
  
  // Check for inactive connections
  for (auto& device : _connectedDevices) {
    if (device.lastActivityTime > 0) {
      unsigned long inactiveTime = now - device.lastActivityTime;
      if (inactiveTime > INACTIVITY_TIMEOUT_MS) {
        Serial.printf("[%lu] [BT] Device %s inactive for %lu ms, disconnecting\n", millis(), device.address.c_str(), inactiveTime);
        disconnectFromDevice(device.address);
        break;  // List modified, exit loop
      }
    }
  }
}
void BluetoothHIDManager::checkAutoReconnect() {
  // 自动重连已禁用：NimBLE connect() 会阻塞最多30秒，
  // 无论放在主线程还是后台任务都会导致问题（UI卡死或竞争崩溃）。
  // 用户需要手动重连。
  
  // 清理已断开的设备标记
  if (!_enabled) return;
  
  static unsigned long lastCleanup = 0;
  unsigned long now = millis();
  if (now - lastCleanup < 5000) return;
  lastCleanup = now;
  
  DeviceListGuard guard(_modifyingDevices);
  auto it = _connectedDevices.begin();
  while (it != _connectedDevices.end()) {
    if (it->wasConnected && (!it->client || !it->client->isConnected())) {
      Serial.printf("[%lu] [BT] Removing disconnected device: %s (no auto-reconnect)\n", millis(), it->address.c_str());
      it = _connectedDevices.erase(it);
    } else {
      ++it;
    }
  }
}

void BluetoothHIDManager::saveState() {
  Serial.printf("[%lu] [BT] Saving state (stub)\n", millis());
  // Stub: would save paired devices to file
}

void BluetoothHIDManager::loadState() {
  Serial.printf("[%lu] [BT] Loading state (stub)\n", millis());
  // Stub: would load paired devices from file
}

void BluetoothHIDManager::saveLastConnectedDevice(const std::string& address, const std::string& name) {
  Serial.printf("[%lu] [BT] Saving last connected device: %s (%s)\n", millis(), name.c_str(), address.c_str());
  
  // Make sure the directory exists
  SdMan.mkdir("/.crosspoint");
  
  FsFile outputFile;
  if (!SdMan.openFileForWrite("BT", "/.crosspoint/bluetooth.bin", outputFile)) {
    Serial.printf("[%lu] [BT] Failed to open bluetooth.bin for writing\n", millis());
    return;
  }
  
  // Write version
  uint8_t version = 1;
  serialization::writePod(outputFile, version);
  
  // Write device info
  serialization::writeString(outputFile, address);
  serialization::writeString(outputFile, name);
  
  outputFile.close();
  Serial.printf("[%lu] [BT] Last connected device saved successfully\n", millis());
}

bool BluetoothHIDManager::loadLastConnectedDevice(std::string& address, std::string& name) {
  FsFile inputFile;
  if (!SdMan.openFileForRead("BT", "/.crosspoint/bluetooth.bin", inputFile)) {
    Serial.printf("[%lu] [BT] No saved bluetooth device found\n", millis());
    return false;
  }
  
  // Read version
  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version != 1) {
    Serial.printf("[%lu] [BT] Unknown bluetooth.bin version: %d\n", millis(), version);
    inputFile.close();
    return false;
  }
  
  // Read device info
  serialization::readString(inputFile, address);
  serialization::readString(inputFile, name);
  
  inputFile.close();
  if (address.empty()) {
    Serial.printf("[%lu] [BT] Loaded bluetooth.bin but address field is empty\n", millis());
    return false;
  }
  Serial.printf("[%lu] [BT] Loaded last connected device: %s (%s)\n", millis(), name.c_str(), address.c_str());
  return true;
}

