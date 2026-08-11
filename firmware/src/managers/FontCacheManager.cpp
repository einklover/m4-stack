#include "FontCacheManager.h"

// 静态成员定义
const esp_partition_t* FontCacheManager::partition = nullptr;
const void* FontCacheManager::mappedPtr = nullptr;
esp_partition_mmap_handle_t FontCacheManager::mmapHandle = 0;
FontCacheManager::CacheHeader FontCacheManager::header = {};
bool FontCacheManager::initialized = false;
bool FontCacheManager::mmapReady = false;

bool FontCacheManager::begin() {
  if (initialized) return partition != nullptr;

  initialized = true;
  memset(&header, 0, sizeof(header));

  // 查找 fontdata 分区（标签 "spiffs"，子类型 0x40）
  partition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA,
      static_cast<esp_partition_subtype_t>(0x40),
      PARTITION_LABEL);

  if (!partition) {
    Serial.printf("[FontCache] ERROR: Partition '%s' not found\n", PARTITION_LABEL);
    return false;
  }

  Serial.printf("[FontCache] Found partition: label=%s, size=%u bytes (%.1f MB)\n",
                partition->label, partition->size, partition->size / 1048576.0);

  // 尝试读取 header
  if (readHeader()) {
    Serial.printf("[FontCache] Cache valid: %u fonts, %u bytes data\n",
                  header.fontCount, header.totalDataSize);
    // 尝试 mmap
    if (doMmap()) {
      Serial.printf("[FontCache] mmap ready, base=%p\n", mappedPtr);
      return true;
    }
  } else {
    Serial.printf("[FontCache] No valid cache found (fresh or cleared)\n");
  }

  return true;
}

bool FontCacheManager::readHeader() {
  if (!partition) return false;

  esp_err_t err = esp_partition_read(partition, 0, &header, sizeof(header));
  if (err != ESP_OK) {
    Serial.printf("[FontCache] Header read error: %s\n", esp_err_to_name(err));
    return false;
  }

  // 验证 magic
  if (memcmp(header.magic, "FNTC", 4) != 0) {
    memset(&header, 0, sizeof(header));
    return false;
  }

  if (header.version != 1 || header.fontCount > MAX_FONTS) {
    Serial.printf("[FontCache] Invalid header: ver=%u, count=%u\n",
                  header.version, header.fontCount);
    memset(&header, 0, sizeof(header));
    return false;
  }

  return header.fontCount > 0;
}

bool FontCacheManager::writeHeader() {
  if (!partition) return false;

  // 擦除第一个扇区（4KB）
  esp_err_t err = esp_partition_erase_range(partition, 0, HEADER_SIZE);
  if (err != ESP_OK) {
    Serial.printf("[FontCache] Header erase error: %s\n", esp_err_to_name(err));
    return false;
  }

  err = esp_partition_write(partition, 0, &header, sizeof(header));
  if (err != ESP_OK) {
    Serial.printf("[FontCache] Header write error: %s\n", esp_err_to_name(err));
    return false;
  }

  return true;
}

bool FontCacheManager::doMmap() {
  if (!partition || header.fontCount == 0) return false;

  unmapIfNeeded();

  // 计算需要映射的总大小（从分区开头到最后一个字体数据结尾）
  uint32_t mapSize = HEADER_SIZE;  // 至少包含 header
  for (uint32_t i = 0; i < header.fontCount; i++) {
    uint32_t end = header.entries[i].offset + header.entries[i].size;
    if (end > mapSize) mapSize = end;
  }

  // 对齐到 64KB（SPI_FLASH_MMU_PAGE_SIZE）
  mapSize = (mapSize + 0xFFFF) & ~0xFFFF;
  if (mapSize > partition->size) mapSize = partition->size;

  esp_err_t err = esp_partition_mmap(
      partition, 0, mapSize,
      ESP_PARTITION_MMAP_DATA,
      &mappedPtr, &mmapHandle);

  if (err != ESP_OK) {
    Serial.printf("[FontCache] mmap error: %s (size=%u)\n", esp_err_to_name(err), mapSize);
    mappedPtr = nullptr;
    mmapReady = false;
    return false;
  }

  mmapReady = true;
  Serial.printf("[FontCache] mmap OK: %u bytes mapped at %p\n", mapSize, mappedPtr);
  return true;
}

void FontCacheManager::unmapIfNeeded() {
  if (mmapReady && mmapHandle) {
    esp_partition_munmap(mmapHandle);
    mmapHandle = 0;
    mappedPtr = nullptr;
    mmapReady = false;
  }
}

bool FontCacheManager::cacheFonts(const String sdPaths[], const int versions[], size_t count) {
  if (!partition) {
    if (!begin()) return false;
  }
  if (!partition || count == 0 || count > MAX_FONTS) return false;

  unmapIfNeeded();

  Serial.printf("[FontCache] Caching %u fonts to flash...\n", count);
  unsigned long totalStart = millis();

  // 计算总大小，确定每个字体的偏移
  // 先扫描所有文件获取大小
  uint32_t sizes[MAX_FONTS] = {};
  for (size_t i = 0; i < count; i++) {
    FsFile f;
    if (!SdMan.openFileForRead("FontCache", sdPaths[i].c_str(), f)) {
      Serial.printf("[FontCache] ERROR: Cannot open %s\n", sdPaths[i].c_str());
      return false;
    }
    sizes[i] = f.fileSize();
    f.close();
  }

  // 计算偏移（从 HEADER_SIZE 开始，每个字体 4 字节对齐）
  uint32_t currentOffset = HEADER_SIZE;
  for (size_t i = 0; i < count; i++) {
    // 4 字节对齐
    currentOffset = (currentOffset + 3) & ~3;
    header.entries[i].offset = currentOffset;
    header.entries[i].size = sizes[i];
    header.entries[i].pathHash = hashPath(sdPaths[i]);
    header.entries[i].fontVersion = versions[i];
    currentOffset += sizes[i];
  }

  uint32_t totalDataEnd = currentOffset;

  // 擦除需要的 flash 区域（按 4KB 扇区对齐）
  uint32_t eraseSize = (totalDataEnd + 4095) & ~4095;
  if (eraseSize > partition->size) {
    Serial.printf("[FontCache] ERROR: Data too large (%u > %u)\n", eraseSize, partition->size);
    return false;
  }

  Serial.printf("[FontCache] Erasing %u bytes...\n", eraseSize);
  esp_err_t err = esp_partition_erase_range(partition, 0, eraseSize);
  if (err != ESP_OK) {
    Serial.printf("[FontCache] Erase error: %s\n", esp_err_to_name(err));
    return false;
  }

  // 逐个复制字体文件到 flash
  static uint8_t buf[4096];  // 4KB 缓冲区

  for (size_t i = 0; i < count; i++) {
    FsFile f;
    if (!SdMan.openFileForRead("FontCache", sdPaths[i].c_str(), f)) {
      Serial.printf("[FontCache] ERROR: Cannot reopen %s\n", sdPaths[i].c_str());
      return false;
    }

    uint32_t writeOffset = header.entries[i].offset;
    uint32_t remaining = sizes[i];
    unsigned long fontStart = millis();

    while (remaining > 0) {
      size_t toRead = (remaining > sizeof(buf)) ? sizeof(buf) : remaining;
      size_t bytesRead = f.read(buf, toRead);
      if (bytesRead == 0) break;

      // esp_partition_write 需要 4 字节对齐的大小
      size_t writeSize = (bytesRead + 3) & ~3;
      // 清零 padding 部分
      if (writeSize > bytesRead) {
        memset(buf + bytesRead, 0, writeSize - bytesRead);
      }

      err = esp_partition_write(partition, writeOffset, buf, writeSize);
      if (err != ESP_OK) {
        Serial.printf("[FontCache] Write error at offset %u: %s\n", writeOffset, esp_err_to_name(err));
        f.close();
        return false;
      }

      writeOffset += bytesRead;
      remaining -= bytesRead;
    }
    f.close();

    unsigned long fontElapsed = millis() - fontStart;
    Serial.printf("[FontCache] Font %u: %u bytes written in %lu ms (%.1f KB/s)\n",
                  i, sizes[i], fontElapsed,
                  fontElapsed > 0 ? (sizes[i] / 1024.0) / (fontElapsed / 1000.0) : 0);
  }

  // 写入 header
  memcpy(header.magic, "FNTC", 4);
  header.version = 1;
  header.fontCount = count;
  header.totalDataSize = totalDataEnd - HEADER_SIZE;

  if (!writeHeader()) {
    Serial.printf("[FontCache] ERROR: Failed to write header\n");
    return false;
  }

  unsigned long totalElapsed = millis() - totalStart;
  Serial.printf("[FontCache] All %u fonts cached in %lu ms (%.1f KB total)\n",
                count, totalElapsed, header.totalDataSize / 1024.0);

  // mmap
  return doMmap();
}

const uint8_t* FontCacheManager::getMappedBase(size_t index) {
  if (!mmapReady || !mappedPtr || index >= header.fontCount) return nullptr;
  return (const uint8_t*)mappedPtr + header.entries[index].offset;
}

const FontCacheManager::FontEntry* FontCacheManager::getEntry(size_t index) {
  if (index >= header.fontCount) return nullptr;
  return &header.entries[index];
}

void FontCacheManager::clearCache() {
  if (!partition) {
    if (!begin()) return;
  }
  if (!partition) return;

  unmapIfNeeded();

  Serial.printf("[FontCache] Clearing cache (erasing header)...\n");
  // 只需擦除第一个扇区即可使 header 无效
  esp_partition_erase_range(partition, 0, HEADER_SIZE);
  memset(&header, 0, sizeof(header));

  Serial.printf("[FontCache] Cache cleared\n");
}

int FontCacheManager::findCached(const String& sdPath) {
  if (header.fontCount == 0) return -1;

  uint32_t h = hashPath(sdPath);
  for (uint32_t i = 0; i < header.fontCount; i++) {
    if (header.entries[i].pathHash == h) {
      return (int)i;
    }
  }
  return -1;
}

uint32_t FontCacheManager::hashPath(const String& path) {
  uint32_t hash = 5381;
  for (int i = 0; i < path.length(); i++) {
    hash = ((hash << 5) + hash) + path[i];
  }
  return hash;
}
