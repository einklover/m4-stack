#include "ReadingStatsStore.h"

#include <Arduino.h>
#include <SDCardManager.h>
#include <Serialization.h>

#include <cstdio>
#include <cstring>

#include "I18n.h"

// 静态实例
ReadingStatsStore ReadingStatsStore::instance;

// ReadingStats 实现

bool ReadingStats::saveToFile() const {
  // 确保目录存在
  SdMan.mkdir("/.crosspoint");

  FsFile outputFile;
  if (!SdMan.openFileForWrite("RST", STATS_FILE, outputFile)) {
    return false;
  }

  // 写入总阅读时长
  serialization::writePod(outputFile, totalReadingTime);
  outputFile.close();
  return true;
}

bool ReadingStats::loadFromFile() {
  FsFile inputFile;
  if (!SdMan.openFileForRead("RST", STATS_FILE, inputFile)) {
    // 文件不存在，使用默认值
    totalReadingTime = 0;
    return false;
  }

  // 读取总阅读时长
  serialization::readPod(inputFile, totalReadingTime);
  inputFile.close();
  return true;
}

void ReadingStats::startSession() {
  sessionStartTime = millis();
  sessionReadingTime = 0;
}

void ReadingStats::endSession() {
  if (sessionStartTime > 0) {
    unsigned long currentTime = millis();
    // 计算本次阅读时长（秒），使用无符号减法避免溢出问题
    sessionReadingTime = (int)((currentTime - sessionStartTime) / 1000);
    totalReadingTime += sessionReadingTime;
    sessionStartTime = 0;
  }
}

void ReadingStats::reset() {
  totalReadingTime = 0;
  sessionReadingTime = 0;
  sessionStartTime = 0;
}

// ReadingStatsStore 实现

std::string ReadingStatsStore::formatReadingTime(int seconds) {
  if (seconds < 0) {
    seconds = 0;
  }
  
  int days = seconds / 86400;
  int hours = (seconds % 86400) / 3600;
  int minutes = (seconds % 3600) / 60;
  int secs = seconds % 60;
  
  char buffer[64];
  buffer[0] = '\0';
  
  if (days > 0) {
    snprintf(buffer + strlen(buffer), sizeof(buffer) - strlen(buffer), "%d%s", days, L(Str::kDayUnit));
  }
  if (hours > 0) {
    snprintf(buffer + strlen(buffer), sizeof(buffer) - strlen(buffer), "%d%s", hours, L(Str::kHourUnit));
  }
  if (minutes > 0) {
    snprintf(buffer + strlen(buffer), sizeof(buffer) - strlen(buffer), "%d%s", minutes, L(Str::kMinuteUnit));
  }
  // 如果时长为0，显示"0分钟"
  if (days == 0 && hours == 0 && minutes == 0) {
    if (secs > 0) {
      snprintf(buffer + strlen(buffer), sizeof(buffer) - strlen(buffer), "%d%s", secs, L(Str::kSecondUnit));
    } else {
      strcpy(buffer, L(Str::kZeroMinutes));
    }
  }
  
  return std::string(buffer);
}
