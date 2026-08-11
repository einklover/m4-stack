#pragma once

#include <Arduino.h>
#include <string>
#include <vector>
#include <functional>
#include <map>
#include "DeviceProfiles.h"

// Forward declarations
class NimBLEClient;
class NimBLERemoteCharacteristic;
class NimBLEAdvertisedDevice;
class ClientCallbacks;  // Forward declaration for friend

struct BluetoothDevice {
  std::string address;
  std::string name;
  int rssi;
  bool isHID = false;
};

struct ConnectedDevice {
  std::string address;
  std::string name;
  NimBLEClient* client = nullptr;
  std::vector<NimBLERemoteCharacteristic*> reportChars;
  bool subscribed = false;
  unsigned long lastActivityTime = 0;  // Timestamp of last HID report received
  uint8_t lastHIDKeycode = 0x00;       // Track last keycode to detect press/release transitions
  unsigned long lastInjectionTime = 0; // Cooldown for button injection to prevent flooding
  bool wasConnected = false;           // Track if this device was previously connected for auto-reconnect
  bool lastButtonState = false;        // Track button pressed state (from byte[0])
  const DeviceProfiles::DeviceProfile* profile = nullptr;  // Device-specific HID profile
  // Hold-sequence tracking: updated on press edge, NOT reset on release
  // Lets the reader distinguish genuine 2-second hold from occasional taps
  unsigned long holdSequenceStartMs = 0;  // First press in current hold sequence
  unsigned long lastPressEventMs = 0;     // Most recent press event time (for gap detection)
};

class BluetoothHIDManager {
public:
  // Singleton access
  static BluetoothHIDManager& getInstance();

  // Allow ClientCallbacks to access private members
  friend class ClientCallbacks;

  // Lifecycle
  bool enable();
  bool disable();
  bool isEnabled() const { return _enabled; }

  // Scanning
  void startScan(uint32_t durationMs = 10000);
  void stopScan();
  bool isScanning() const { return _scanning; }
  const std::vector<BluetoothDevice>& getDiscoveredDevices() const { return _discoveredDevices; }

  // Connection
  bool connectToDevice(const std::string& address);
  /**
   * Attempt to connect up to `maxAttempts` times before giving up.
   * This is useful for UI code that may try to connect to non‑responsive
   * devices and must not crash the system.
   */
  bool connectToDeviceWithRetries(const std::string& address, int maxAttempts = 3);
  bool disconnectFromDevice(const std::string& address);
  bool isConnected(const std::string& address) const;
  std::vector<std::string> getConnectedDevices() const;

  // Input handling
  void processInputEvents();
  void setInputCallback(std::function<void(uint16_t keycode)> callback);
  void setButtonInjector(std::function<void(uint8_t buttonIndex)> injector);
  void updateActivity();  // Call periodically to check inactivity timeout
  void checkAutoReconnect();  // Auto-reconnect to previously connected devices if disconnected
  
  // 蓝牙断开通知：主线程调用 consumeDisconnectNotify() 检查并清除标志
  bool consumeDisconnectNotify() {
    if (_disconnectNotify) {
      _disconnectNotify = false;
      return true;
    }
    return false;
  }
  
  // Check if BLE has had activity recently (within last 4 minutes)
  // Used by power manager to prevent sleep during BLE use
  bool hasRecentActivity() const;

  // Key learning: returns and clears the last raw keycode received from BT device
  // Used by the key mapping UI to capture what button the user pressed
  uint8_t consumeLastKeycode();

  // Hold duration for the current continuous BT press sequence (ms).
  // Resets to 0 when no press event has been seen for > 1000 ms.
  // Reader uses this to require a genuine 2-second hold before chapter skip.
  unsigned long getBTHoldDurationMs() const { return _btHoldDurationMs; }

  // State persistence
  void saveState();
  void loadState();
  void saveLastConnectedDevice(const std::string& address, const std::string& name);
  bool loadLastConnectedDevice(std::string& address, std::string& name);

  std::string lastError;
  //防止崩溃，让外部可读取信息
  void onScanResult(NimBLEAdvertisedDevice* advertisedDevice);

private:

  // BLE callbacks (public for NimBLE callbacks)
  
  static void onHIDNotify(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify);

private:
  BluetoothHIDManager();
  ~BluetoothHIDManager();
  BluetoothHIDManager(const BluetoothHIDManager&) = delete;
  BluetoothHIDManager& operator=(const BluetoothHIDManager&) = delete;

  void cleanup();
  uint16_t parseHIDReport(uint8_t* data, size_t length);
  ConnectedDevice* findConnectedDevice(const std::string& address);
  uint8_t mapKeycodeToButton(uint8_t keycode, const DeviceProfiles::DeviceProfile* profile);

  bool _enabled = false;
  bool _scanning = false;
  std::vector<BluetoothDevice> _discoveredDevices;
  std::vector<ConnectedDevice> _connectedDevices;
  std::function<void(uint16_t)> _inputCallback;
  std::function<void(uint8_t)> _buttonInjector;
  
  // Last raw keycode captured from BT device (for key learning UI)
  volatile uint8_t _lastRawKeycode = 0x00;
  volatile unsigned long _btHoldDurationMs = 0;  // Tracks current BT hold sequence duration
  
  // Guard flag: set to true while main thread is modifying _connectedDevices
  // onHIDNotify (NimBLE thread) checks this to avoid use-after-free crashes
  volatile bool _modifyingDevices = false;
  volatile bool _disconnectNotify = false;  // NimBLE回调线程设置，主线程消费
  
  // RAII guard for _modifyingDevices flag
  struct DeviceListGuard {
    volatile bool& flag;
    DeviceListGuard(volatile bool& f) : flag(f) { flag = true; }
    ~DeviceListGuard() { flag = false; }
  };
  
  // Inactivity timeout (milliseconds)
  static constexpr unsigned long INACTIVITY_TIMEOUT_MS = 120000;  // 2 minutes
  unsigned long lastMaintenanceCheck = 0;
  unsigned long _scanStartTime = 0;
  uint32_t _scanDuration = 0;
};
