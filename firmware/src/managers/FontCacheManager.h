#pragma once

#include <Arduino.h>
#include <esp_partition.h>
#include <SDCardManager.h>

/**
 * 字体缓存管理器 - Flash 分区内存映射方案
 *
 * 将 SD 卡中的字体文件写入 raw flash 分区，然后通过 esp_partition_mmap()
 * 映射到 CPU 地址空间，实现与内置字体完全一致的 O(1) 指针访问速度。
 */
class FontCacheManager {
 public:
  static constexpr size_t MAX_FONTS = 16;
  static constexpr size_t HEADER_SIZE = 4096;  // 1 flash sector
  static constexpr const char* PARTITION_LABEL = "spiffs";  // 分区标签

  // 分区 header 中每个字体的元信息
  struct FontEntry {
    uint32_t offset;       // 字体数据在分区中的偏移
    uint32_t size;         // 字体文件大小
    uint32_t pathHash;     // SD 卡路径的哈希值（用于匹配）
    uint32_t fontVersion;  // 字体文件版本 (0 or 1)
  };

  // 分区 header
  struct CacheHeader {
    char magic[4];           // "FNTC"
    uint32_t version;        // Header 版本 = 1
    uint32_t fontCount;      // 缓存的字体数量
    uint32_t totalDataSize;  // 所有字体数据的总大小
    FontEntry entries[MAX_FONTS];
  };

  /**
   * 初始化：查找分区，读取 header，如果有缓存则 mmap
   */
  static bool begin();

  /**
   * 批量缓存字体到 flash 分区
   * 会擦除分区，从 SD 卡复制所有字体文件，然后 mmap
   *
   * @param sdPaths SD 卡字体路径数组
   * @param versions 每个字体的版本号数组
   * @param count 字体数量
   * @return 成功返回 true
   */
  static bool cacheFonts(const String sdPaths[], const int versions[], size_t count);

  /**
   * 获取第 index 个字体在 mmap 内存中的基址指针
   */
  static const uint8_t* getMappedBase(size_t index);

  /**
   * 获取字体的元信息
   */
  static const FontEntry* getEntry(size_t index);

  /**
   * 获取缓存的字体数量
   */
  static size_t getFontCount() { return header.fontCount; }

  /**
   * 清除缓存（擦除 header 扇区）
   */
  static void clearCache();

  /**
   * 检查某个 SD 路径的字体是否已缓存
   * @return 缓存索引，未找到返回 -1
   */
  static int findCached(const String& sdPath);

  /**
   * mmap 是否就绪
   */
  static bool isReady() { return mmapReady; }

  /**
   * 字体数据是否已写入 flash（即使 mmap 失败也返回 true）
   */
  static bool isCached() { return initialized && header.fontCount > 0; }

  /**
   * 获取分区指针（用于 esp_partition_read fallback）
   */
  static const esp_partition_t* getPartition() { return partition; }

  /**
   * 计算路径哈希（DJB2）
   */
  static uint32_t hashPath(const String& path);

 private:
  static const esp_partition_t* partition;
  static const void* mappedPtr;
  static esp_partition_mmap_handle_t mmapHandle;
  static CacheHeader header;
  static bool initialized;
  static bool mmapReady;

  static bool readHeader();
  static bool writeHeader();
  static bool doMmap();
  static void unmapIfNeeded();
};
