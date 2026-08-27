#include "FontManager.h"

#include <GfxRenderer.h>  // for EpdFontData usage validation if needed
#include <HardwareSerial.h>
#include <SDCardManager.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CustomEpdFont.h"
#include "FontCacheManager.h"
#include "TtfEpdFont.h"
#include "util/M4FontDebugPolicy.h"

namespace {
constexpr const char* kLegacyEpdFontDir = "/fonts";
constexpr const char* kRuntimeTtfDir = "/FONT";
FontManager::RuntimeFontDiagnostic gRuntimeFontDiagnostic;

bool isRuntimeFontName(const String& name) {
  return M4FontDebugPolicy::isRuntimeFilename(name.c_str());
}

const char* runtimeFontType(const char* filename) {
  const char* dot = strrchr(filename ? filename : "", '.');
  return dot ? dot + 1 : "";
}

uint16_t readU16BE(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t readU32BE(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

bool inspectSfntTables(FsFile& file, size_t fileSize, uint32_t faceOffset) {
  constexpr uint16_t kMaxTables = 128;
  if (faceOffset > fileSize || fileSize - faceOffset < 12) return false;
  uint8_t header[12] = {};
  if (!file.seekSet(faceOffset) || file.read(header, sizeof(header)) != sizeof(header)) return false;
  const uint32_t version = readU32BE(header);
  if (version != 0x00010000u && version != 0x74727565u && version != 0x4f54544fu) return false;
  const uint16_t tableCount = readU16BE(header + 4);
  const uint64_t directoryEnd = uint64_t(faceOffset) + 12u + uint64_t(tableCount) * 16u;
  if (tableCount == 0 || tableCount > kMaxTables || directoryEnd > fileSize) return false;

  uint8_t record[16] = {};
  for (uint16_t i = 0; i < tableCount; ++i) {
    const uint32_t recordOffset = faceOffset + 12u + uint32_t(i) * 16u;
    if (!file.seekSet(recordOffset) || file.read(record, sizeof(record)) != sizeof(record)) return false;
    const uint64_t tableOffset = readU32BE(record + 8);
    const uint64_t tableLength = readU32BE(record + 12);
    if (tableOffset > fileSize || tableLength > fileSize - tableOffset) return false;
  }
  return true;
}

void inspectRuntimeFont(FsFile& file, FontManager::RuntimeFontInfo& info) {
  const size_t fileSize = static_cast<size_t>(file.fileSize());
  if (fileSize == 0) {
    strncpy(info.signature, "empty", sizeof(info.signature) - 1);
    strncpy(info.integrity, "empty", sizeof(info.integrity) - 1);
    return;
  }
  uint8_t signature[4] = {};
  if (!file.seekSet(0) || file.read(signature, sizeof(signature)) != sizeof(signature)) {
    strncpy(info.signature, "short", sizeof(info.signature) - 1);
    strncpy(info.integrity, "header_truncated", sizeof(info.integrity) - 1);
    return;
  }

  const uint32_t sfntVersion = readU32BE(signature);
  if (sfntVersion == 0x00010000u) {
    strncpy(info.signature, "sfnt", sizeof(info.signature) - 1);
  } else if (memcmp(signature, "true", 4) == 0) {
    strncpy(info.signature, "true", sizeof(info.signature) - 1);
  } else if (memcmp(signature, "OTTO", 4) == 0) {
    strncpy(info.signature, "OTTO", sizeof(info.signature) - 1);
  } else if (memcmp(signature, "ttcf", 4) == 0) {
    strncpy(info.signature, "ttcf", sizeof(info.signature) - 1);
    uint8_t collectionHeader[12] = {};
    if (!file.seekSet(0) || file.read(collectionHeader, sizeof(collectionHeader)) != sizeof(collectionHeader) ||
        fileSize < 16) {
      strncpy(info.integrity, "header_truncated", sizeof(info.integrity) - 1);
      return;
    }
    const uint32_t faceCount = readU32BE(collectionHeader + 8);
    if (faceCount == 0 || faceCount > 64 || 12u + uint64_t(faceCount) * 4u > fileSize) {
      strncpy(info.integrity, "directory_past_eof", sizeof(info.integrity) - 1);
      return;
    }
    uint8_t rawOffset[4] = {};
    for (uint32_t i = 0; i < faceCount; ++i) {
      if (!file.seekSet(12u + i * 4u) || file.read(rawOffset, sizeof(rawOffset)) != sizeof(rawOffset)) break;
      if (inspectSfntTables(file, fileSize, readU32BE(rawOffset))) {
        strncpy(info.integrity, "tables_in_file", sizeof(info.integrity) - 1);
        return;
      }
    }
    strncpy(info.integrity, "table_past_eof", sizeof(info.integrity) - 1);
    return;
  } else if (memcmp(signature, "wOFF", 4) == 0) {
    strncpy(info.signature, "wOFF", sizeof(info.signature) - 1);
    strncpy(info.integrity, "not_sfnt", sizeof(info.integrity) - 1);
    return;
  } else if (memcmp(signature, "wOF2", 4) == 0) {
    strncpy(info.signature, "wOF2", sizeof(info.signature) - 1);
    strncpy(info.integrity, "not_sfnt", sizeof(info.integrity) - 1);
    return;
  } else {
    strncpy(info.signature, "other", sizeof(info.signature) - 1);
    strncpy(info.integrity, "not_sfnt", sizeof(info.integrity) - 1);
    return;
  }

  strncpy(info.integrity, inspectSfntTables(file, fileSize, 0) ? "tables_in_file" : "table_past_eof",
          sizeof(info.integrity) - 1);
}

const char* diagnosticStage(const char* error) {
  if (!error) return "load";
  if (strstr(error, "cmap")) return "cmap";
  if (strstr(error, "CFF") || strstr(error, "CharString") || strstr(error, "Type2")) return "cff";
  if (strstr(error, "glyph") || strstr(error, "glyf") || strstr(error, "loca")) return "glyph";
  if (strstr(error, "table") || strstr(error, "sfnt") || strstr(error, "head") || strstr(error, "maxp") ||
      strstr(error, "hhea") || strstr(error, "hmtx")) return "table";
  if (strstr(error, "alloc") || strstr(error, "memory") || strstr(error, "OOM") || strstr(error, "PSRAM")) {
    return "memory";
  }
  if (strstr(error, "file") || strstr(error, "open")) return "file";
  return "load";
}
}  // namespace

FontManager& FontManager::getInstance() {
  static FontManager instance;
  return instance;
}

FontManager::~FontManager() {
  for (auto& familyPair : loadedFonts) {
    for (auto& sizePair : familyPair.second) {
      delete sizePair.second;
    }
  }
}

void FontManager::appendFontDiagnostic(const char* line) {
#if defined(M4_FONT_DIAGNOSTIC) && M4_FONT_DIAGNOSTIC
  if (!line || !*line) return;
  constexpr const char* kLogDir = "/.crosspoint/logs";
  constexpr const char* kLogPath = "/.crosspoint/logs/font_debug.log";
  constexpr size_t kMaxLogBytes = 64u * 1024u;

  SdMan.mkdir(kLogDir, true);
  FsFile existing;
  if (SdMan.openFileForRead("FontDiag", kLogPath, existing)) {
    const bool rotate = existing.fileSize() >= kMaxLogBytes;
    existing.close();
    if (rotate) SdMan.remove(kLogPath);
  }

  FsFile f = SdMan.open(kLogPath, O_WRONLY | O_CREAT | O_APPEND);
  if (!f) return;
  f.write(reinterpret_cast<const uint8_t*>(line), strlen(line));
  f.write(reinterpret_cast<const uint8_t*>("\n"), 1);
  f.close();
#else
  // Release/user builds deliberately avoid diagnostic SD writes. Runtime TTF
  // performs many seeks/reads and logging those operations competes with font
  // I/O, adds latency and grows files indefinitely. Enable explicitly with
  // -DM4_FONT_DIAGNOSTIC=1 on a developer build when a trace is needed.
  (void)line;
#endif
}

// Bridge used by the reusable TTF SD stream without coupling lib/EpdFont to
// firmware application headers.
void m4AppendFontDiagnostic(const char* line) { FontManager::appendFontDiagnostic(line); }

void FontManager::clearLoadedFonts() {
  // 注意：不 delete EpdFontFamily/EpdFont 指针，因为 GfxRenderer::fontMap
  // 中仍持有 EpdFontFamily 的值拷贝，其内部的 EpdFont* 指向同一对象。
  // 少量内存泄漏（几百字节），在嵌入式设备上可接受（字体切换是低频操作）。
  // TTF 后端例外：释放其位图缓存，避免多尺寸实例的 PSRAM 累积。
  for (auto& familyPair : loadedFonts) {
    for (auto& sizePair : familyPair.second) {
      if (EpdFontFamily* fam = sizePair.second) {
        if (const EpdFont* font = fam->getFont(EpdFontFamily::REGULAR)) {
          if (font->isRuntimeTtf()) {
            const TtfEpdFont* ttf = static_cast<const TtfEpdFont*>(font);
            const_cast<TtfEpdFont*>(ttf)->clearCaches();
          }
        }
      }
    }
  }
  loadedFonts.clear();
}

const std::vector<std::string>& FontManager::getAvailableFamilies() {
  if (!scanned) {
    scanFonts();
  }
  return availableFamilies;
}

const std::vector<std::string>& FontManager::getAvailableTtfFamilies() {
  if (!scanned) {
    scanFonts();
  }
  return availableTtfFamilies;
}

const std::vector<FontManager::RuntimeFontInfo>& FontManager::getRuntimeFonts() {
  if (!scanned) scanFonts();
  return runtimeFonts;
}

FontManager::RuntimeFontDiagnostic FontManager::lastRuntimeFontDiagnostic() {
  return gRuntimeFontDiagnostic;
}

void FontManager::scanFonts() {
  Serial.println("[FM] Scanning fonts...");
  availableFamilies.clear();
  availableTtfFamilies.clear();
  runtimeFonts.clear();
  scanned = true;

  auto scanDir = [&](const char* dirPath, bool allowLegacyEpdFont) {
    FsFile fontDir;
    if (!SdMan.openFileForRead("FontScan", dirPath, fontDir)) {
      Serial.printf("[FM] Failed to open %s directory\n", dirPath);
      return;
    }
    if (!fontDir.isDirectory()) {
      Serial.printf("[FM] %s is not a directory\n", dirPath);
      fontDir.close();
      return;
    }

    Serial.printf("[FM] %s opened. Iterating files...\n", dirPath);
    FsFile file;
    while (file.openNext(&fontDir, O_READ)) {
      if (!file.isDirectory()) {
        char filename[128];
        file.getName(filename, sizeof(filename));
        Serial.printf("[FM] Checking %s/%s\n", dirPath, filename);

        String name = String(filename);
        if (allowLegacyEpdFont && name.endsWith(".epdfont")) {
        // Use the full filename (minus .epdfont extension) as the font name
        String fontName = name.substring(0, name.length() - 8);

        if (fontName.length() > 0) {
          if (std::find(availableFamilies.begin(), availableFamilies.end(), fontName.c_str()) ==
              availableFamilies.end()) {
            availableFamilies.push_back(fontName.c_str());
            Serial.printf("[FM] Added font: %s\n", fontName.c_str());
          }
        }
        } else if (isRuntimeFontName(name)) {
        // Runtime sfnt/collection: preserve the full filename so the settings
        // UI exposes the exact file the user dropped into /FONT.
        if (std::find(availableFamilies.begin(), availableFamilies.end(), name.c_str()) ==
            availableFamilies.end()) {
          RuntimeFontInfo info;
          info.filename = name.c_str();
          info.displayName = name.c_str();
          info.type = runtimeFontType(name.c_str());
          info.sizeBytes = static_cast<uint32_t>(file.fileSize());
          inspectRuntimeFont(file, info);
          if (!M4FontDebugPolicy::isCompleteRuntimeFont(info.sizeBytes, info.signature, info.integrity)) {
            Serial.printf("[FM] Skipping incomplete runtime font: %s size=%u signature=%s integrity=%s\n",
                          name.c_str(), static_cast<unsigned>(info.sizeBytes), info.signature, info.integrity);
          } else {
            availableFamilies.push_back(name.c_str());
            availableTtfFamilies.push_back(name.c_str());
            runtimeFonts.push_back(std::move(info));
            Serial.printf("[FM] Added runtime font: %s\n", name.c_str());
          }
        }
      }
      }
      file.close();
    }
    fontDir.close();
  };

  // Legacy generated bitmap fonts stay in /fonts for internal compatibility.
  scanDir(kLegacyEpdFontDir, true);
  // User-provided runtime sfnt files/collections live in the device's
  // documented uppercase FONT directory.
  scanDir(kRuntimeTtfDir, false);

  std::sort(availableFamilies.begin(), availableFamilies.end());
  std::sort(runtimeFonts.begin(), runtimeFonts.end(),
            [](const RuntimeFontInfo& a, const RuntimeFontInfo& b) { return a.filename < b.filename; });
  Serial.printf("[FM] Scan complete. Found %d families\n", availableFamilies.size());
}

// 解析字体文件头，返回解析结果。不创建字体对象。
struct ParsedFontHeader {
  bool valid = false;
  int version = -1;
  uint32_t intervalCount = 0;
  uint32_t glyphCount = 0;
  uint32_t offsetIntervals = 0;
  uint32_t offsetGlyphs = 0;
  uint32_t offsetBitmaps = 0;
  uint8_t advanceY = 0;
  int32_t ascender = 0;
  int32_t descender = 0;
  bool is2Bit = false;
};

static ParsedFontHeader parseFontHeader(const String& path) {
  ParsedFontHeader result;

  FsFile f;
  if (!SdMan.openFileForRead("FontLoading", path.c_str(), f)) {
    Serial.printf("[FontHdr] Cannot open file: %s\n", path.c_str());
    return result;
  }

  uint32_t fileSize = f.fileSize();
  uint32_t buf[12];  // 48 bytes
  if (f.read(buf, 48) != 48) {
    Serial.printf("[FontHdr] File too small (%u bytes): %s\n", fileSize, path.c_str());
    f.close();
    return result;
  }
  f.close();

  if (strncmp((char*)&buf[0], "EPDF", 4) != 0) {
    uint8_t* b = (uint8_t*)buf;
    Serial.printf("[FontHdr] Bad magic: 0x%02X%02X%02X%02X (expected 'EPDF'): %s\n",
                  b[0], b[1], b[2], b[3], path.c_str());
    return result;
  }

  // Helper lambdas for parsing V0 and V1 headers
  auto parseV1 = [&]() {
    uint8_t* b8 = (uint8_t*)buf;
    result.is2Bit = (b8[6] != 0);
    result.advanceY = b8[8];
    result.ascender = (int8_t)b8[9];
    result.descender = (int8_t)b8[10];
    result.intervalCount = b8[12] | (b8[13] << 8) | (b8[14] << 16) | (b8[15] << 24);
    result.glyphCount = b8[16] | (b8[17] << 8) | (b8[18] << 16) | (b8[19] << 24);
    result.offsetIntervals = b8[20] | (b8[21] << 8) | (b8[22] << 16) | (b8[23] << 24);
    result.offsetGlyphs = b8[24] | (b8[25] << 8) | (b8[26] << 16) | (b8[27] << 24);
    result.offsetBitmaps = b8[28] | (b8[29] << 8) | (b8[30] << 16) | (b8[31] << 24);
  };

  auto parseV0 = [&]() {
    result.intervalCount = buf[1];
    result.advanceY = buf[3];
    result.ascender = (int32_t)buf[5];
    result.descender = (int32_t)buf[7];
    result.is2Bit = (buf[8] != 0);
    result.offsetIntervals = buf[9];
    result.offsetGlyphs = buf[10];
    result.offsetBitmaps = buf[11];
  };

  // Validate parsed offsets: all must be non-zero, ordered, and within file size
  auto offsetsValid = [&]() -> bool {
    return result.offsetIntervals > 0 &&
           result.offsetGlyphs > result.offsetIntervals &&
           result.offsetBitmaps > result.offsetGlyphs &&
           result.offsetBitmaps < fileSize;
  };

  // Version Detection with fallback
  // V1 heuristic: byte 4 = version(1), bytes 20-23 = offsetIntervals(typically 32)
  // V0 heuristic: bytes 36-39 = offsetIntervals(typically 48)
  // Problem: V0 files with intervalCount=1 and ascender=32 can false-match V1.
  // Solution: try V1 first, validate offsets, fall back to V0 if invalid.
  bool detected = false;

  if (buf[5] == 32 && (buf[1] & 0xFFFF) == 1) {
    // Candidate V1 — parse and validate
    result.version = 1;
    parseV1();
    if (offsetsValid()) {
      detected = true;
    } else {
      Serial.printf("[FontHdr] V1 candidate failed validation, trying V0...\n");
      result = ParsedFontHeader();  // reset
    }
  }

  if (!detected) {
    if (buf[9] == 48 || buf[2] > 10000) {
      result.version = 0;
      parseV0();
      if (offsetsValid()) {
        detected = true;
      }
    }
  }

  if (!detected) {
    // Last resort: also try V0 even if heuristics didn't match, just validate
    result = ParsedFontHeader();
    result.version = 0;
    parseV0();
    if (offsetsValid()) {
      Serial.printf("[FontHdr] Heuristics missed, but V0 offsets valid\n");
      detected = true;
    }
  }

  if (!detected) {
    Serial.printf("[FontHdr] All format detection failed: %s\n", path.c_str());
    uint8_t* raw = (uint8_t*)buf;
    for (int row = 0; row < 3; row++) {
      Serial.printf("  [%02d]", row * 16);
      for (int col = 0; col < 16; col++) {
        Serial.printf(" %02X", raw[row * 16 + col]);
      }
      Serial.println();
    }
    return result;
  }

  Serial.printf("[FontHdr] OK V%d: itvl=%u glyphs=%u bmp=%u cnt=%u advY=%u asc=%d desc=%d\n",
                result.version, result.offsetIntervals, result.offsetGlyphs, result.offsetBitmaps,
                result.intervalCount, result.advanceY, result.ascender, result.descender);

  result.valid = true;
  return result;
}

// V0 格式 mmap 字体：intervals 和 bitmap 直接指针访问，glyph 从 mmap 内存即时转换 13→16 字节
// bitmap 访问走基类 loadGlyphBitmap（O(1) 指针运算），性能与内置字体一致
class MmapV0EpdFont : public EpdFont {
 public:
  MmapV0EpdFont(const EpdFontData* data, const uint8_t* fontBase, uint32_t offsetGlyphs)
      : EpdFont(data), fontBase(fontBase), offsetGlyphs(offsetGlyphs) {}

  const EpdGlyph* getGlyph(uint32_t cp, const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const override {
    const EpdGlyph* result = getGlyphInternal(cp, style);
    if (!result && cp != '?') {
      result = getGlyphInternal('?', style);
    }
    return result;
  }

 private:
  const EpdGlyph* getGlyphInternal(uint32_t cp, const EpdFontStyles::Style style) const {
    const EpdFontData* d = getData(style);
    if (!d) return nullptr;

    const EpdUnicodeInterval* intervals = d->intervals;
    const int count = d->intervalCount;
    if (count == 0) return nullptr;

    // Binary search (same as base class)
    int left = 0, right = count - 1;
    while (left <= right) {
      const int mid = left + (right - left) / 2;
      const EpdUnicodeInterval* interval = &intervals[mid];
      if (cp < interval->first) {
        right = mid - 1;
      } else if (cp > interval->last) {
        left = mid + 1;
      } else {
        // Found! Read 13-byte V0 glyph from mmap memory (no file I/O!)
        uint32_t glyphIndex = interval->offset + (cp - interval->first);
        const uint8_t* buf = fontBase + offsetGlyphs + glyphIndex * 13;
        cachedGlyph.width = buf[0];
        cachedGlyph.height = buf[1];
        cachedGlyph.advanceX = buf[2];
        cachedGlyph.left = (int16_t)(int8_t)buf[3];
        // buf[4] unused
        cachedGlyph.top = (int16_t)(int8_t)buf[5];
        // buf[6] unused
        cachedGlyph.dataLength = (uint32_t)(buf[7] | (buf[8] << 8));
        cachedGlyph.dataOffset = buf[9] | (buf[10] << 8) | (buf[11] << 16) | (buf[12] << 24);
        return &cachedGlyph;
      }
    }
    return nullptr;
  }

  const uint8_t* fontBase;
  uint32_t offsetGlyphs;
  mutable EpdGlyph cachedGlyph;  // 单核 ESP32-C3 无线程安全问题
};

// Flash 分区直读字体：当 mmap 失败时使用（字体数据已在 flash 中）
// 通过 esp_partition_read() 读取，比 SD 卡快 10-100 倍
class FlashReadEpdFont : public EpdFont {
 public:
  FlashReadEpdFont(const EpdFontData* data, const esp_partition_t* part,
                   uint32_t fontOffset, uint32_t offsetGlyphs, uint32_t offsetBitmaps,
                   int version)
      : EpdFont(data), partition(part), fontOffset(fontOffset),
        offsetGlyphs(offsetGlyphs), offsetBitmaps(offsetBitmaps), version(version) {}

  ~FlashReadEpdFont() override {
    free(bitmapBuf);
  }

  const EpdGlyph* getGlyph(uint32_t cp, const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const override {
    const EpdGlyph* result = getGlyphInternal(cp, style);
    if (!result && cp != '?') {
      result = getGlyphInternal('?', style);
    }
    return result;
  }

  const uint8_t* loadGlyphBitmap(const EpdGlyph* glyph, uint8_t* buffer,
                                 const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const override {
    if (!glyph || glyph->dataLength == 0) return nullptr;

    uint32_t absOffset = fontOffset + offsetBitmaps + glyph->dataOffset;

    // 使用调用方提供的 buffer，或内部缓冲区
    uint8_t* dst = buffer;
    if (!dst) {
      if (glyph->dataLength > bitmapBufSize) {
        free(bitmapBuf);
        bitmapBufSize = glyph->dataLength;
        bitmapBuf = (uint8_t*)malloc(bitmapBufSize);
        if (!bitmapBuf) { bitmapBufSize = 0; return nullptr; }
      }
      dst = bitmapBuf;
    }

    esp_partition_read(partition, absOffset, dst, glyph->dataLength);
    return dst;
  }

 private:
  const EpdGlyph* getGlyphInternal(uint32_t cp, const EpdFontStyles::Style style) const {
    const EpdFontData* d = getData(style);
    if (!d) return nullptr;

    const EpdUnicodeInterval* intervals = d->intervals;
    const int count = d->intervalCount;
    if (count == 0) return nullptr;

    int left = 0, right = count - 1;
    while (left <= right) {
      const int mid = left + (right - left) / 2;
      const EpdUnicodeInterval* interval = &intervals[mid];
      if (cp < interval->first) {
        right = mid - 1;
      } else if (cp > interval->last) {
        left = mid + 1;
      } else {
        uint32_t glyphIndex = interval->offset + (cp - interval->first);
        uint32_t stride = (version == 1) ? 16 : 13;
        uint32_t absOffset = fontOffset + offsetGlyphs + glyphIndex * stride;

        uint8_t buf[16] __attribute__((aligned(4))) = {};
        esp_partition_read(partition, absOffset, buf, stride);

        if (version == 1) {
          memcpy(&cachedGlyph, buf, 16);
        } else {
          cachedGlyph.width = buf[0];
          cachedGlyph.height = buf[1];
          cachedGlyph.advanceX = buf[2];
          cachedGlyph.left = (int16_t)(int8_t)buf[3];
          cachedGlyph.top = (int16_t)(int8_t)buf[5];
          cachedGlyph.dataLength = (uint32_t)(buf[7] | (buf[8] << 8));
          cachedGlyph.dataOffset = buf[9] | (buf[10] << 8) | (buf[11] << 16) | (buf[12] << 24);
        }
        return &cachedGlyph;
      }
    }
    return nullptr;
  }

  const esp_partition_t* partition;
  uint32_t fontOffset;       // 字体数据在分区中的偏移
  uint32_t offsetGlyphs;     // glyph 区在字体文件中的偏移
  uint32_t offsetBitmaps;    // bitmap 区在字体文件中的偏移
  int version;
  mutable EpdGlyph cachedGlyph;
  mutable uint8_t* bitmapBuf = nullptr;
  mutable size_t bitmapBufSize = 0;
};

// 从 mmap 指针创建 EpdFont（V0 和 V1 均支持）
static EpdFont* createMmapFont(const uint8_t* fontBase, const ParsedFontHeader& hdr) {
  EpdFontData* fontData = new (std::nothrow) EpdFontData();
  if (!fontData) return nullptr;

  fontData->intervals = (const EpdUnicodeInterval*)(fontBase + hdr.offsetIntervals);
  fontData->bitmap = fontBase + hdr.offsetBitmaps;
  fontData->intervalCount = hdr.intervalCount;
  fontData->advanceY = hdr.advanceY;
  fontData->ascender = hdr.ascender;
  fontData->descender = hdr.descender;
  fontData->is2Bit = hdr.is2Bit;

  if (hdr.version == 1) {
    // V1: glyph 条目恰好 16 字节 = EpdGlyph 结构体，直接 cast
    fontData->glyph = (const EpdGlyph*)(fontBase + hdr.offsetGlyphs);
    Serial.printf("[FontMgr] Created mmap V1 font: intervals=%u, glyphs@%p, bitmap@%p\n",
                  hdr.intervalCount, fontData->glyph, fontData->bitmap);
    return new EpdFont(fontData);
  } else {
    // V0: glyph 条目 13 字节，通过 MmapV0EpdFont 即时转换（仍是 O(1) 指针运算）
    fontData->glyph = nullptr;  // V0 不能直接 cast
    Serial.printf("[FontMgr] Created mmap V0 font: intervals=%u, bitmap@%p (glyph on-the-fly)\n",
                  hdr.intervalCount, fontData->bitmap);
    return new MmapV0EpdFont(fontData, fontBase, hdr.offsetGlyphs);
  }
}

// 从 Flash 分区直读创建字体（mmap 失败时的 fallback，比 SD 卡快 10-100x）
static EpdFont* createFlashFont(const FontCacheManager::FontEntry* entry,
                                const ParsedFontHeader& hdr) {
  const esp_partition_t* part = FontCacheManager::getPartition();
  if (!part || !entry) return nullptr;

  // 从 flash 读取 intervals 到 RAM（很小：12 bytes × intervalCount）
  EpdUnicodeInterval* intervals = new (std::nothrow) EpdUnicodeInterval[hdr.intervalCount];
  if (!intervals) return nullptr;

  esp_err_t err = esp_partition_read(part, entry->offset + hdr.offsetIntervals,
                                     intervals, hdr.intervalCount * sizeof(EpdUnicodeInterval));
  if (err != ESP_OK) {
    delete[] intervals;
    return nullptr;
  }

  EpdFontData* fontData = new (std::nothrow) EpdFontData();
  if (!fontData) { delete[] intervals; return nullptr; }

  fontData->intervals = intervals;
  fontData->intervalCount = hdr.intervalCount;
  fontData->glyph = nullptr;
  fontData->bitmap = nullptr;
  fontData->advanceY = hdr.advanceY;
  fontData->ascender = hdr.ascender;
  fontData->descender = hdr.descender;
  fontData->is2Bit = hdr.is2Bit;

  Serial.printf("[FontMgr] Created flash-read V%d font: intervals=%u (partition read fallback)\n",
                hdr.version, hdr.intervalCount);

  return new FlashReadEpdFont(fontData, part, entry->offset,
                              hdr.offsetGlyphs, hdr.offsetBitmaps, hdr.version);
}

// 从 SD 卡文件创建 CustomEpdFont（最终 fallback）
static EpdFont* createSdFont(const String& path, const ParsedFontHeader& hdr) {
  // 需要加载 intervals 到 RAM
  FsFile f;
  if (!SdMan.openFileForRead("FontLoading", path.c_str(), f)) {
    return nullptr;
  }

  EpdUnicodeInterval* intervals = new (std::nothrow) EpdUnicodeInterval[hdr.intervalCount];
  if (!intervals) {
    f.close();
    return nullptr;
  }

  if (!f.seekSet(hdr.offsetIntervals)) {
    delete[] intervals;
    f.close();
    return nullptr;
  }

  f.read((uint8_t*)intervals, hdr.intervalCount * sizeof(EpdUnicodeInterval));
  f.close();

  EpdFontData* fontData = new (std::nothrow) EpdFontData();
  if (!fontData) {
    delete[] intervals;
    return nullptr;
  }
  fontData->intervalCount = hdr.intervalCount;
  fontData->intervals = intervals;
  fontData->glyph = nullptr;
  fontData->bitmap = nullptr;
  fontData->advanceY = hdr.advanceY;
  fontData->ascender = hdr.ascender;
  fontData->descender = hdr.descender;
  fontData->is2Bit = hdr.is2Bit;

  Serial.printf("[FontMgr] Created SD font (fallback): %s\n", path.c_str());
  return new CustomEpdFont(path, fontData, hdr.offsetIntervals, hdr.offsetGlyphs, hdr.offsetBitmaps, hdr.version);
}

// Helper to load a single font file (tries mmap first, falls back to SD)
static EpdFont* loadFontFile(const String& path) {
  Serial.printf("[FontMgr] Loading file: %s\n", path.c_str());

  ParsedFontHeader hdr = parseFontHeader(path);
  if (!hdr.valid) {
    Serial.printf("[FontMgr] Invalid or missing: %s\n", path.c_str());
    return nullptr;
  }

  Serial.printf("[FontMgr] Parsed: V%d, intervals=%u, is2Bit=%d\n",
                hdr.version, hdr.intervalCount, hdr.is2Bit);

  // 尝试 mmap 路径（V0 和 V1 都支持）
  if (FontCacheManager::isReady()) {
    int idx = FontCacheManager::findCached(path);
    if (idx >= 0) {
      const uint8_t* fontBase = FontCacheManager::getMappedBase(idx);
      if (fontBase) {
        EpdFont* font = createMmapFont(fontBase, hdr);
        if (font) {
          Serial.printf("[FontMgr] Using mmap font [%d]: %s\n", idx, path.c_str());
          return font;
        }
      }
    }
  }

  // mmap 失败但字体已缓存到 flash → 使用 esp_partition_read（比 SD 卡快 10-100x）
  if (!FontCacheManager::isReady() && FontCacheManager::isCached()) {
    int idx = FontCacheManager::findCached(path);
    if (idx >= 0) {
      const FontCacheManager::FontEntry* entry = FontCacheManager::getEntry(idx);
      if (entry) {
        EpdFont* font = createFlashFont(entry, hdr);
        if (font) {
          Serial.printf("[FontMgr] Using flash-read font [%d]: %s\n", idx, path.c_str());
          return font;
        }
      }
    }
  }

  // Fallback 到 SD 卡文件读取
  return createSdFont(path, hdr);
}

EpdFontFamily* FontManager::getCustomFontFamily(const std::string& familyName, int fontSize) {
  if (loadedFonts[familyName][fontSize]) {
    return loadedFonts[familyName][fontSize];
  }

  const bool isRuntimeFont = isRuntimeFontName(String(familyName.c_str()));

  if (isRuntimeFont) {
    gRuntimeFontDiagnostic = {};
    gRuntimeFontDiagnostic.attempted = true;
    strncpy(gRuntimeFontDiagnostic.filename, familyName.c_str(), sizeof(gRuntimeFontDiagnostic.filename) - 1);
    // Runtime sfnt/collection: streamed glyf rasterizer, no flash caching. The
    // runtime face is per (family, size) so changing customFontSize recreates it.
    String fontPath = String(kRuntimeTtfDir) + "/" + String(familyName.c_str());
    Serial.printf("[FontMgr] Loading runtime font: %s @%dpx\n", fontPath.c_str(), fontSize);
    char diag[256];
    snprintf(diag, sizeof(diag), "load_begin family=%s size=%d path=%s", familyName.c_str(), fontSize,
             fontPath.c_str());
    appendFontDiagnostic(diag);

    TtfEpdFont* regular = new (std::nothrow) TtfEpdFont(fontPath, (uint16_t)fontSize);
    if (regular && regular->valid()) {
      gRuntimeFontDiagnostic.ok = true;
      strncpy(gRuntimeFontDiagnostic.stage, "ready", sizeof(gRuntimeFontDiagnostic.stage) - 1);
      EpdFontFamily* fontFamily = new EpdFontFamily(regular, nullptr, nullptr, nullptr);
      loadedFonts[familyName][fontSize] = fontFamily;
      snprintf(diag, sizeof(diag), "load_ok family=%s size=%d", familyName.c_str(), fontSize);
      appendFontDiagnostic(diag);
      return fontFamily;
    }
    const char* err = regular ? regular->lastError() : "alloc failed";
    strncpy(gRuntimeFontDiagnostic.stage, diagnosticStage(err), sizeof(gRuntimeFontDiagnostic.stage) - 1);
    strncpy(gRuntimeFontDiagnostic.error, err ? err : "load failed", sizeof(gRuntimeFontDiagnostic.error) - 1);
    Serial.printf("[FontMgr] Failed to load runtime font: %s (%s)\n", fontPath.c_str(), err);
    snprintf(diag, sizeof(diag), "load_fail family=%s size=%d error=%s", familyName.c_str(), fontSize, err);
    appendFontDiagnostic(diag);
    delete regular;
    return nullptr;
  }

  // familyName 即字体列表中显示的完整名称（文件名去掉 .epdfont 后缀）
  // 直接加载 /fonts/<familyName>.epdfont，不探测 Regular/Bold/Italic 变体
  String fontPath = "/fonts/" + String(familyName.c_str()) + ".epdfont";
  Serial.printf("[FontMgr] Loading font: %s\n", fontPath.c_str());

  // === 阶段1: 缓存到 flash ===
  if (FontCacheManager::isReady() || FontCacheManager::begin()) {
    if (FontCacheManager::findCached(fontPath) < 0) {
      ParsedFontHeader hdr = parseFontHeader(fontPath);
      if (hdr.valid) {
        String paths[1] = { fontPath };
        int versions[1] = { hdr.version };
        Serial.printf("[FontMgr] Caching font to flash...\n");
        FontCacheManager::cacheFonts(paths, versions, 1);
      }
    }
  }

  // === 阶段2: 加载字体（优先 mmap，fallback SD） ===
  EpdFont* regular = loadFontFile(fontPath);

  if (regular) {
    EpdFontFamily* fontFamily = new EpdFontFamily(regular, nullptr, nullptr, nullptr);
    loadedFonts[familyName][fontSize] = fontFamily;
    return fontFamily;
  }

  Serial.printf("[FontMgr] Failed to load font: %s\n", fontPath.c_str());
  return nullptr;
}
