#pragma once
#include <cstdint>
#include <string>

// 阅读统计数据
struct ReadingStats {
  int totalReadingTime = 0;    // 总阅读时长（秒）
  int sessionReadingTime = 0;  // 本次阅读时长（秒）
  unsigned long sessionStartTime = 0;  // 本次开始时间（millis）
  
  // 阅读统计文件路径
  static constexpr const char* STATS_FILE = "/.crosspoint/reading_stats.bin";
  
  bool saveToFile() const;
  bool loadFromFile();
  void startSession();   // 开始阅读会话
  void endSession();     // 结束阅读会话，累计时间
  void reset();          // 重置统计
};

// 全局阅读统计管理器
class ReadingStatsStore {
  static ReadingStatsStore instance;
  ReadingStats stats;
  
public:
  static ReadingStatsStore& getInstance() { return instance; }
  
  void startSession() { stats.startSession(); }
  void endSession() { 
    stats.endSession(); 
    saveToFile(); 
  }
  void saveToFile() const { stats.saveToFile(); }
  bool loadFromFile() { return stats.loadFromFile(); }
  
  int getTotalReadingTime() const { return stats.totalReadingTime; }
  int getSessionReadingTime() const { return stats.sessionReadingTime; }
  
  void reset() { stats.reset(); saveToFile(); }
  
  // 格式化阅读时长为友好字符串（如 "2天5小时34分钟"）
  static std::string formatReadingTime(int seconds);
};

#define READING_STATS ReadingStatsStore::getInstance()
