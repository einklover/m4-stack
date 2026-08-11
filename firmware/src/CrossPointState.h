#pragma once
#include <iosfwd>
#include <string>

class CrossPointState {
  // Static instance
  static CrossPointState instance;

 public:
  std::string openEpubPath;
  uint8_t lastSleepImage;
  uint8_t readerActivityLoadCount = 0;
  bool lastSleepFromReader = false;
  float pendingBookmarkPercent = -1.0f;  // >=0 时表示有待跳转的书签百分比
  bool ntpSyncedThisBoot = false;  // 标记本次开机是否已同步过NTP时间
  ~CrossPointState() = default;

  // Get singleton instance
  static CrossPointState& getInstance() { return instance; }

  bool saveToFile() const;

  bool loadFromFile();
  bool isRenderComplete = false;
};

// Helper macro to access settings
#define APP_STATE CrossPointState::getInstance()
