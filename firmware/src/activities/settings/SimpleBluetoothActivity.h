#pragma once

#include <GfxRenderer.h>

#include <atomic>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "MappedInputManager.h"
#include "BluetoothHIDManager.h"

enum class BtPageState {
  MAIN_MENU,
  DEVICE_LIST,
  CONNECTING,
  CONNECTED,
  CONNECTION_FAILED,
  KEY_MAPPING,
  KEY_LEARNING,
  KEY_ACTION_SELECT
};

class SimpleBluetoothActivity : public Activity {
 public:
  explicit SimpleBluetoothActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                   const std::function<void()>& onComplete)
      : Activity("SimpleBluetooth", renderer, mappedInput), onComplete(onComplete) {}

  void onEnter() override;
  void loop() override;
  void onExit() override;

 private:
  BluetoothHIDManager* btMgr = nullptr;
  
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  
  BtPageState state = BtPageState::MAIN_MENU;
  int selectedIndex = 0;
  std::vector<BluetoothDevice> devices;
  std::string selectedDeviceAddress;
  std::string connectionError;
  bool scanJustStarted = false;  // 关键修复：标记扫描刚启动，避免误判为扫描完成
  
  // 按键映射状态
  int keyMappingSelectedSlot = 0;
  int keyActionSelectedIndex = 0;
  uint8_t learntKeycode = 0x00;
  
  std::atomic<bool> updateRequired{false};
  
  static void taskTrampoline(void* param);
  void displayTaskLoop();
  void render();
  
  void renderMainMenu();
  void renderScanning();
  void renderDeviceList();
  void renderConnecting();
  void renderConnected();
  void renderConnectionFailed();
  void renderKeyMapping();
  void renderKeyLearning();
  void renderKeyActionSelect();
  
  void startScan();
  void connectToDevice(int index);
  void updateDeviceList();
  
  // 按键映射辅助函数
  void handleMainMenuInput();
  void handleDeviceListInput();
  void handleConnectedInput();
  void handleConnectionFailedInput();
  void handleKeyMappingInput();
  void handleKeyLearningInput();
  void handleKeyActionInput();
  
  const std::function<void()> onComplete;
};
