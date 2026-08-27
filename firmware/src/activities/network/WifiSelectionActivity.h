#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "activities/ActivityWithSubactivity.h"
#include "apps/M4WifiFailureTracker.h"

// Structure to hold WiFi network information
struct WifiNetworkInfo {
  std::string ssid;
  int32_t rssi;
  bool isEncrypted;
  bool hasSavedPassword;  // Whether we have saved credentials for this network
};

// WiFi selection states
enum class WifiSelectionState {
  SCANNING,           // Scanning for networks
  NETWORK_LIST,       // Displaying available networks
  PASSWORD_ENTRY,     // Entering password for selected network
  CONNECTING,         // Attempting to connect
  CONNECTED,          // Successfully connected
  CONNECTION_FAILED   // Connection failed
};

/**
 * WifiSelectionActivity is responsible for scanning WiFi APs and connecting to them.
 * It will:
 * - Enter scanning mode on entry
 * - List available WiFi networks
 * - Allow selection and launch KeyboardEntryActivity for password if needed
 * - Persist newly entered credentials after a successful connection
 * - Call onComplete callback when connected or cancelled
 *
 * The onComplete callback receives true if connected successfully, false if cancelled.
 */
class WifiSelectionActivity final : public ActivityWithSubactivity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;
  WifiSelectionState state = WifiSelectionState::SCANNING;
  int selectedNetworkIndex = 0;
  std::vector<WifiNetworkInfo> networks;
  const std::function<void(bool connected)> onComplete;

  // Selected network for connection
  std::string selectedSSID;
  bool selectedRequiresPassword = false;

  // Connection result
  std::string connectedIP;
  std::string connectionError;

  // Password from the current keyboard entry or saved credentials
  std::string enteredPassword;

  // Cached MAC address string for display
  std::string cachedMacAddress;

  // Whether network was connected using a saved password
  bool usedSavedPassword = false;

  M4WifiFailureTracker failureTracker;

  // Connection timeout
  static constexpr unsigned long CONNECTION_TIMEOUT_MS = 15000;
  unsigned long connectionStartTime = 0;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;
  void renderNetworkList() const;
  void renderPasswordEntry() const;
  void renderConnecting() const;
  void renderConnected() const;
  void renderConnectionFailed() const;

  void startWifiScan();
  void processWifiScanResults();
  void selectNetwork(int index);
  void attemptConnection();
  void checkConnectionStatus();
  std::string getSignalStrengthIndicator(int32_t rssi) const;

 public:
  explicit WifiSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 const std::function<void(bool connected)>& onComplete)
      : ActivityWithSubactivity("WifiSelection", renderer, mappedInput), onComplete(onComplete) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;

  // Get the IP address after successful connection
  const std::string& getConnectedIP() const { return connectedIP; }
};
