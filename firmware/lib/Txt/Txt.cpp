#include "Txt.h"

#include <FsHelpers.h>
#include <JpegToBmpConverter.h>
#include <Serialization.h>

#include <cstring>

#include "GbkToUtf8.h"
#include "M4TxtEncoding.h"

// ============================================================================
// GBK/GB2312 to UTF-8 conversion helper for chapter title extraction
// ============================================================================
static std::string convertGbkToUtf8(const char* buf, size_t len, uint8_t& carry, bool& hasCarry) {
  return gbkChunkToUtf8(reinterpret_cast<const uint8_t*>(buf), len, getGbkTable(), carry, hasCarry);
}

// ============================================================================
// 章节行检测：判断一行文本是否为章节标题
// 支持三种模式（参考正则表达式）：
//   1. 中文：第X章/节/卷/部/篇/回/集/季/幕/册/辑/话（UTF-8 和 GBK 编码）
//   2. 数字编号：1、 1. 1） 1】 等格式
//   3. 英文：Chapter/Section/Part/Book/Episode/Scene/Act + 数字
// ============================================================================
static bool isChapterLine(const char* s, int len) {
    if (len < 2) return false;

    // ---------- 跳过行首空白和装饰符号 ----------
    // 对应正则中的 ^\s*[#*→>=\-\_【([{＜]*
    int i = 0;
    while (i < len) {
        uint8_t c = (uint8_t)s[i];
        // ASCII 空白和装饰符
        if (c == ' ' || c == '\t' || c == '#' || c == '*' ||
            c == '>' || c == '=' || c == '-' || c == '_' ||
            c == '(' || c == '[' || c == '{') {
            i++;
            continue;
        }
        // UTF-8 三字节装饰符：→(E2 86 92) 【(E3 80 90) ＜(EF BC 9C)
        if (c >= 0xE0 && i + 2 < len) {
            uint32_t cp = ((c & 0x0F) << 12) |
                          (((uint8_t)s[i+1] & 0x3F) << 6) |
                          ((uint8_t)s[i+2] & 0x3F);
            if (cp == 0x2192 || cp == 0x3010 || cp == 0xFF1C) {
                i += 3;
                continue;
            }
        }
        break;
    }

    if (i >= len) return false;

    // ========== 模式1：中文"第X章/节/卷/..." ==========

    // --- UTF-8 编码："第" = E7 AC AC ---
    if (i + 2 < len && (uint8_t)s[i] == 0xE7 && (uint8_t)s[i+1] == 0xAC && (uint8_t)s[i+2] == 0xAC) {
        i += 3; // 跳过"第"
        // 跳过数字部分（中文数字或阿拉伯数字）
        bool hasNumber = false;
        while (i < len) {
            uint8_t c = (uint8_t)s[i];
            // 阿拉伯数字 0-9
            if (c >= '0' && c <= '9') { hasNumber = true; i++; continue; }
            // UTF-8 中文数字：零一二三四五六七八九十百千万
            if (c >= 0xE0 && i + 2 < len) {
                uint32_t cp = ((c & 0x0F) << 12) |
                              (((uint8_t)s[i+1] & 0x3F) << 6) |
                              ((uint8_t)s[i+2] & 0x3F);
                // 零(96F6) 一(4E00) 二(4E8C) 三(4E09) 四(56DB) 五(4E94)
                // 六(516D) 七(4E03) 八(516B) 九(4E5D) 十(5341) 百(767E)
                // 千(5343) 万(4E07)
                if (cp == 0x96F6 || cp == 0x4E00 || cp == 0x4E8C || cp == 0x4E09 ||
                    cp == 0x56DB || cp == 0x4E94 || cp == 0x516D || cp == 0x4E03 ||
                    cp == 0x516B || cp == 0x4E5D || cp == 0x5341 || cp == 0x767E ||
                    cp == 0x5343 || cp == 0x4E07) {
                    hasNumber = true; i += 3; continue;
                }
            }
            break;
        }
        if (!hasNumber) return false;
        // 检测章节后缀字符：章节卷部篇回集季幕册辑话
        if (i + 2 < len && (uint8_t)s[i] >= 0xE0) {
            uint32_t cp = (((uint8_t)s[i] & 0x0F) << 12) |
                          (((uint8_t)s[i+1] & 0x3F) << 6) |
                          ((uint8_t)s[i+2] & 0x3F);
            // 章(7AE0) 节(8282) 卷(5377) 部(90E8) 篇(7BC7) 回(56DE)
            // 集(96C6) 季(5B63) 幕(5E55) 册(518C) 辑(8F91) 话(8BDD)
            if (cp == 0x7AE0 || cp == 0x8282 || cp == 0x5377 || cp == 0x90E8 ||
                cp == 0x7BC7 || cp == 0x56DE || cp == 0x96C6 || cp == 0x5B63 ||
                cp == 0x5E55 || cp == 0x518C || cp == 0x8F91 || cp == 0x8BDD) {
                return true;
            }
        }
        return false;
    }

    // --- GBK 编码："第" = B5 DA ---
    if (i + 1 < len && (uint8_t)s[i] == 0xB5 && (uint8_t)s[i+1] == 0xDA) {
        // 在后续字节中查找 GBK 章节后缀
        // 章(D5C2) 节(BDC2) 卷(BED2) 部(B2BF) 篇(C6AA) 回(BBD8) 集(BCAF)
        for (int j = i + 2; j + 1 < len; j++) {
            uint8_t c1 = (uint8_t)s[j], c2 = (uint8_t)s[j+1];
            if ((c1 == 0xD5 && c2 == 0xC2) || (c1 == 0xBD && c2 == 0xC2) ||
                (c1 == 0xBE && c2 == 0xD2) || (c1 == 0xB2 && c2 == 0xBF) ||
                (c1 == 0xC6 && c2 == 0xAA) || (c1 == 0xBB && c2 == 0xD8) ||
                (c1 == 0xBC && c2 == 0xAF)) {
                return true;
            }
        }
        return false;
    }

    // ========== 模式2：数字编号 "1、" "1." "1）" "1】" ==========
    if ((uint8_t)s[i] >= '0' && (uint8_t)s[i] <= '9') {
        int numStart = i;
        while (i < len && (uint8_t)s[i] >= '0' && (uint8_t)s[i] <= '9') i++;
        if (i > numStart && i < len) {
            uint8_t next = (uint8_t)s[i];
            // ASCII 分隔符：. )
            if (next == '.' || next == ')') return true;
            // UTF-8 分隔符：、(E3 80 81) ）(EF BC 89) 】(E3 80 91)
            if (next >= 0xE0 && i + 2 < len) {
                uint32_t cp = ((next & 0x0F) << 12) |
                              (((uint8_t)s[i+1] & 0x3F) << 6) |
                              ((uint8_t)s[i+2] & 0x3F);
                if (cp == 0x3001 || cp == 0xFF09 || cp == 0x3011) return true;
            }
        }
        return false;
    }

    // ========== 模式3：英文章节关键词 ==========
    // 不区分大小写的关键词匹配辅助函数
    auto matchKeyword = [&](const char* keyword, int kwLen) -> bool {
        if (i + kwLen > len) return false;
        for (int j = 0; j < kwLen; j++) {
            char a = s[i + j];
            char b = keyword[j];
            // 转小写比较
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) return false;
        }
        return true;
    };

    // 检测英文关键词后跟空格和数字
    // allowDot: 是否允许关键词后带点号（如 Ch. Sec. Ep.）
    auto checkEnglishChapter = [&](const char* kw, int kwLen, bool allowDot) -> bool {
        if (!matchKeyword(kw, kwLen)) return false;
        int pos = i + kwLen;
        // 可选的点号（Ch. Sec. Ep.）
        if (allowDot && pos < len && s[pos] == '.') pos++;
        // 必须跟空格
        if (pos >= len || s[pos] != ' ') return false;
        pos++;
        // 跳过额外空格
        while (pos < len && s[pos] == ' ') pos++;
        // 必须跟数字
        return (pos < len && (uint8_t)s[pos] >= '0' && (uint8_t)s[pos] <= '9');
    };

    if (checkEnglishChapter("chapter", 7, false)) return true;
    if (checkEnglishChapter("ch", 2, true)) return true;
    if (checkEnglishChapter("section", 7, false)) return true;
    if (checkEnglishChapter("sec", 3, true)) return true;
    if (checkEnglishChapter("part", 4, false)) return true;
    if (checkEnglishChapter("book", 4, false)) return true;
    if (checkEnglishChapter("episode", 7, false)) return true;
    if (checkEnglishChapter("ep", 2, true)) return true;
    if (checkEnglishChapter("scene", 5, false)) return true;
    if (checkEnglishChapter("act", 3, false)) return true;

    return false;
}

Txt::Txt(std::string path, std::string cacheBasePath)
    : filepath(std::move(path)), effectivePath(this->filepath),
      cacheBasePath(std::move(cacheBasePath)) {
  // Generate cache path from file path hash
  const size_t hash = std::hash<std::string>{}(this->filepath);
  cachePath = this->cacheBasePath + "/txt_" + std::to_string(hash);
}

std::string Txt::getUtf8CachePath() const {
  return cachePath + "/utf8.txt";
}

bool Txt::convertGbkToUtf8Cache() {
  setupCacheDir();
  
  const std::string utf8Path = getUtf8CachePath();
  
  // 已存在则跳过
  if (SdMan.exists(utf8Path.c_str())) {
    Serial.printf("[%lu] [TXT] UTF-8 cache already exists\n", millis());
    return true;
  }
  
  Serial.printf("[%lu] [TXT] Converting GBK → UTF-8 cache...\n", millis());
  
  FsFile src, dst;
  if (!SdMan.openFileForRead("TXT", filepath, src)) return false;
  if (!SdMan.openFileForWrite("TXT", utf8Path, dst)) { src.close(); return false; }
  
  const uint16_t* table = getGbkTable();
  uint8_t gbkCarry = 0;
  bool gbkHasCarry = false;
  
  static uint8_t readBuf[1024];
  while (src.available()) {
    int got = src.read(readBuf, sizeof(readBuf));
    if (got <= 0) break;
    
    std::string utf8 = gbkChunkToUtf8(readBuf, static_cast<size_t>(got), table, gbkCarry, gbkHasCarry);
    dst.write(reinterpret_cast<const uint8_t*>(utf8.c_str()), utf8.size());
  }
  
  src.close();
  dst.sync();
  dst.close();
  
  Serial.printf("[%lu] [TXT] GBK → UTF-8 conversion complete: %s\n", millis(), utf8Path.c_str());
  return true;
}

const char* Txt::getEncodingName() const {
  switch (encodingType) {
    case TXT_ENCODING_UTF8:
      return contentBomSkip ? "utf-8-bom" : "utf-8";
    case TXT_ENCODING_GBK:
      return "gbk";
    case TXT_ENCODING_UTF16LE:
      return "utf-16le";
    case TXT_ENCODING_UTF16BE:
      return "utf-16be";
    default:
      return "unknown";
  }
}

bool Txt::load() {
  if (loaded) {
    return true;
  }

  if (!SdMan.exists(filepath.c_str())) {
    Serial.printf("[%lu] [TXT] File does not exist: %s\n", millis(), filepath.c_str());
    return false;
  }

  FsFile file;
  if (!SdMan.openFileForRead("TXT", filepath, file)) {
    Serial.printf("[%lu] [TXT] Failed to open file: %s\n", millis(), filepath.c_str());
    return false;
  }

  fileSize = file.size();
  // Direct-read policy: always use original file. Never rewrite source or create
  // whole-book UTF-8 sidecars here. Old utf8.txt caches are left untouched.
  effectivePath = filepath;
  contentBomSkip = 0;
  encodingType = TXT_ENCODING_UTF8;
  encodingDiagnostic.clear();

  // Bounded multipoint sampling: head ≤1024 + mid ≤256+pad + tail ≤256+pad.
  // Avoids mis-labeling pure-ASCII front matter as UTF-8 when GBK body follows.
  M4TxtEncoding::GbkLookupFn lookupFn = [](uint8_t lead, uint8_t trail) -> uint16_t {
    return ::gbkLookup(getGbkTable(), lead, trail);
  };
  M4TxtEncoding::DetectResult det{};
  constexpr size_t kHeadMax = 1024;
  constexpr size_t kBlock = 256;
  constexpr size_t kAlignPad = 3;
  static uint8_t headBuf[kHeadMax];
  static uint8_t midBuf[kBlock + kAlignPad];
  static uint8_t tailBuf[kBlock + kAlignPad];

  file.seek(0);
  const size_t headLen = static_cast<size_t>(file.read(headBuf, fileSize < kHeadMax ? fileSize : kHeadMax));
  if (headLen == 0) {
    file.close();
    loaded = true;
    encodingDiagnostic = "empty_sample_default_utf8";
    Serial.printf("[%lu] [TXT] Loaded empty/unreadable: %s\n", millis(), filepath.c_str());
    return true;
  }

  if (fileSize <= headLen) {
    // Whole file in head buffer
    det = M4TxtEncoding::detectMulti(headBuf, headLen, lookupFn);
  } else {
    // Assemble a virtual multipoint image is heavy; call detectOne + merge manually.
    M4TxtEncoding::DetectSampleOpts headOpt;
    headOpt.isHead = true;
    headOpt.isCompleteFile = false;
    headOpt.fileSize = fileSize;
    det = M4TxtEncoding::detectOne(headBuf, headLen, lookupFn, headOpt);

    if (det.enc != M4TxtEncoding::TxtEnc::Utf8Bom && det.enc != M4TxtEncoding::TxtEnc::Utf16Le &&
        det.enc != M4TxtEncoding::TxtEnc::Utf16Be) {
      int totalPairs = 0, totalBad = 0;
      bool anyNotUtf8 = false;
      bool anyGb18030 = false;

      auto accBuf = [&](const uint8_t* p, size_t n, bool align) {
        size_t off = align ? M4TxtEncoding::alignSampleStart(p, n) : 0;
        if (off >= n) return;
        const uint8_t* q = p + off;
        const size_t m = n - off;
        if (M4TxtEncoding::countGb18030FourByte(q, m) >= 1) anyGb18030 = true;
        if (!M4TxtEncoding::isStrictUtf8(q, m)) anyNotUtf8 = true;
        const auto gs = M4TxtEncoding::scoreGbkDetail(q, m, lookupFn);
        totalPairs += gs.pairs;
        totalBad += gs.bad;
        if (gs.fourByte > 0) anyGb18030 = true;
      };

      accBuf(headBuf, headLen, false);

      // Mid
      size_t midCenter = fileSize / 2;
      size_t midStart = midCenter > kAlignPad ? midCenter - kAlignPad : 0;
      if (midStart + kBlock + kAlignPad > fileSize) {
        midStart = fileSize > (kBlock + kAlignPad) ? fileSize - (kBlock + kAlignPad) : 0;
      }
      size_t midWant = fileSize - midStart;
      if (midWant > kBlock + kAlignPad) midWant = kBlock + kAlignPad;
      file.seek(midStart);
      const size_t midGot = static_cast<size_t>(file.read(midBuf, midWant));
      if (midGot > 0) accBuf(midBuf, midGot, true);

      // Tail
      size_t tailStart = fileSize > (kBlock + kAlignPad) ? fileSize - (kBlock + kAlignPad) : 0;
      size_t tailWant = fileSize - tailStart;
      file.seek(tailStart);
      const size_t tailGot = static_cast<size_t>(file.read(tailBuf, tailWant));
      if (tailGot > 0) accBuf(tailBuf, tailGot, true);

      M4TxtEncoding::DetectResult parts[1] = {det};
      det = M4TxtEncoding::mergeDetect(parts, 1, fileSize, totalPairs, totalBad, anyNotUtf8, anyGb18030);
    }
  }
  file.close();

  contentBomSkip = det.bomBytes;
  if (det.diagnostic && det.diagnostic[0]) encodingDiagnostic = det.diagnostic;
  switch (det.enc) {
    case M4TxtEncoding::TxtEnc::Utf8Bom:
    case M4TxtEncoding::TxtEnc::Utf8:
      encodingType = TXT_ENCODING_UTF8;
      break;
    case M4TxtEncoding::TxtEnc::Gbk:
      encodingType = TXT_ENCODING_GBK;
      break;
    case M4TxtEncoding::TxtEnc::Utf16Le:
      encodingType = TXT_ENCODING_UTF16LE;
      break;
    case M4TxtEncoding::TxtEnc::Utf16Be:
      encodingType = TXT_ENCODING_UTF16BE;
      break;
    default:
      encodingType = TXT_ENCODING_UNKNOWN;
      if (encodingDiagnostic.empty()) encodingDiagnostic = "undetected_or_unsupported";
      break;
  }
  Serial.printf("[%lu] [TXT] encoding=%s bom_skip=%u diag=%s supported=%d multipoint (no full convert)\n", millis(),
                det.name, static_cast<unsigned>(contentBomSkip),
                encodingDiagnostic.empty() ? "-" : encodingDiagnostic.c_str(),
                isEncodingSupported() ? 1 : 0);

  loaded = true;
  Serial.printf("[%lu] [TXT] Loaded TXT file: %s (%zu bytes) enc=%s\n", millis(), filepath.c_str(), fileSize,
                getEncodingName());
  return true;
}

std::string Txt::getTitle() const {
  // Extract filename without path and extension
  size_t lastSlash = filepath.find_last_of('/');
  std::string filename = (lastSlash != std::string::npos) ? filepath.substr(lastSlash + 1) : filepath;

  // Remove .txt extension
  if (filename.length() >= 4 && filename.substr(filename.length() - 4) == ".txt") {
    filename = filename.substr(0, filename.length() - 4);
  }

  // 如果文件名是GBK编码，转换为UTF-8
  // 注意：这里使用encodingType判断，而不是依赖文件内容编码
  // 因为文件名编码和文件内容编码可能不同
  if (encodingType == TXT_ENCODING_GBK) {
    const uint16_t* gbkTable = getGbkTable();
    if (gbkTable) {
      uint8_t carry = 0;
      bool hasCarry = false;
      std::string titleUtf8 = gbkChunkToUtf8(
          reinterpret_cast<const uint8_t*>(filename.c_str()),
          filename.size(), gbkTable, carry, hasCarry);
      if (!titleUtf8.empty()) {
        return titleUtf8;
      }
    }
  }

  return filename;
}

void Txt::setupCacheDir() const {
  if (!SdMan.exists(cacheBasePath.c_str())) {
    SdMan.mkdir(cacheBasePath.c_str());
  }
  if (!SdMan.exists(cachePath.c_str())) {
    SdMan.mkdir(cachePath.c_str());
  }
}

std::string Txt::findCoverImage() const {
  // Get the folder containing the txt file
  size_t lastSlash = filepath.find_last_of('/');
  std::string folder = (lastSlash != std::string::npos) ? filepath.substr(0, lastSlash) : "";
  if (folder.empty()) {
    folder = "/";
  }

  // Get the base filename without extension (e.g., "mybook" from "/books/mybook.txt")
  std::string baseName = getTitle();

  // Image extensions to try
  const char* extensions[] = {".bmp", ".jpg", ".jpeg", ".png", ".BMP", ".JPG", ".JPEG", ".PNG"};

  // First priority: look for image with same name as txt file (e.g., mybook.jpg)
  for (const auto& ext : extensions) {
    std::string coverPath = folder + "/" + baseName + ext;
    if (SdMan.exists(coverPath.c_str())) {
      Serial.printf("[%lu] [TXT] Found matching cover image: %s\n", millis(), coverPath.c_str());
      return coverPath;
    }
  }

  // Fallback: look for cover image files
  const char* coverNames[] = {"cover", "Cover", "COVER"};
  for (const auto& name : coverNames) {
    for (const auto& ext : extensions) {
      std::string coverPath = folder + "/" + std::string(name) + ext;
      if (SdMan.exists(coverPath.c_str())) {
        Serial.printf("[%lu] [TXT] Found fallback cover image: %s\n", millis(), coverPath.c_str());
        return coverPath;
      }
    }
  }

  return "";
}

std::string Txt::getCoverBmpPath() const { return cachePath + "/cover.bmp"; }

bool Txt::generateCoverBmp() const {
  // Already generated, return true
  if (SdMan.exists(getCoverBmpPath().c_str())) {
    return true;
  }

  std::string coverImagePath = findCoverImage();
  if (coverImagePath.empty()) {
    Serial.printf("[%lu] [TXT] No cover image found for TXT file\n", millis());
    return false;
  }

  // Setup cache directory
  setupCacheDir();

  // Get file extension
  const size_t len = coverImagePath.length();
  const bool isJpg =
      (len >= 4 && (coverImagePath.substr(len - 4) == ".jpg" || coverImagePath.substr(len - 4) == ".JPG")) ||
      (len >= 5 && (coverImagePath.substr(len - 5) == ".jpeg" || coverImagePath.substr(len - 5) == ".JPEG"));
  const bool isBmp = len >= 4 && (coverImagePath.substr(len - 4) == ".bmp" || coverImagePath.substr(len - 4) == ".BMP");

  if (isBmp) {
    // Copy BMP file to cache
    Serial.printf("[%lu] [TXT] Copying BMP cover image to cache\n", millis());
    FsFile src, dst;
    if (!SdMan.openFileForRead("TXT", coverImagePath, src)) {
      return false;
    }
    if (!SdMan.openFileForWrite("TXT", getCoverBmpPath(), dst)) {
      src.close();
      return false;
    }
    uint8_t buffer[1024];
    while (src.available()) {
      size_t bytesRead = src.read(buffer, sizeof(buffer));
      dst.write(buffer, bytesRead);
    }
    src.close();
    dst.close();
    Serial.printf("[%lu] [TXT] Copied BMP cover to cache\n", millis());
    return true;
  }

  if (isJpg) {
    // Convert JPG/JPEG to BMP (same approach as Epub)
    Serial.printf("[%lu] [TXT] Generating BMP from JPG cover image\n", millis());
    FsFile coverJpg, coverBmp;
    if (!SdMan.openFileForRead("TXT", coverImagePath, coverJpg)) {
      return false;
    }
    if (!SdMan.openFileForWrite("TXT", getCoverBmpPath(), coverBmp)) {
      coverJpg.close();
      return false;
    }
    const bool success = JpegToBmpConverter::jpegFileToBmpStream(coverJpg, coverBmp);
    coverJpg.close();
    coverBmp.close();

    if (!success) {
      Serial.printf("[%lu] [TXT] Failed to generate BMP from JPG cover image\n", millis());
      SdMan.remove(getCoverBmpPath().c_str());
    } else {
      Serial.printf("[%lu] [TXT] Generated BMP from JPG cover image\n", millis());
    }
    return success;
  }

  // PNG files are not supported (would need a PNG decoder)
  Serial.printf("[%lu] [TXT] Cover image format not supported (only BMP/JPG/JPEG)\n", millis());
  return false;
}




bool Txt::readContent(uint8_t* buffer, size_t offset, size_t length, 
                      bool convertToUtf8, size_t* actualLen) const {
  if (!loaded) {
    return false;
  }

  FsFile file;
  if (!SdMan.openFileForRead("TXT", effectivePath, file)) {
    return false;
  }

  if (!file.seek(offset)) {
    file.close();
    return false;
  }

  size_t bytesRead = file.read(buffer, length);
  file.close();

  if (bytesRead == 0) {
    return false;
  }

  if (actualLen) *actualLen = bytesRead;
  return true;
}


//加目录
namespace {
// Must match CACHE_MAGIC / CACHE_VERSION in save/load (0x43484150 / 4).
constexpr uint32_t kChapMagic = 0x43484150u;
constexpr uint32_t kChapVersion = 4u;

void chapterBatchPath(char* out, size_t outLen, const std::string& cachePath, int startChapter) {
  snprintf(out, outLen, "%s/chapters_%d_25.bin", cachePath.c_str(), startChapter);
}

void purgeChapterBatchFile(const char* path, const char* reason) {
  if (!path || !path[0]) return;
  if (!SdMan.exists(path)) return;
  if (SdMan.remove(path)) {
    Serial.printf("[%lu] [TRA] Purged incomplete chapter cache → %s (%s)\n", millis(), path,
                  reason ? reason : "");
  } else {
    Serial.printf("[%lu] [TRA] Failed to purge chapter cache → %s (%s)\n", millis(), path,
                  reason ? reason : "");
  }
}

// Read batch header only. Returns false if missing / unreadable / wrong shape.
bool readChapterBatchHeader(const char* path, uint8_t expectEnc, int expectStart, uint32_t* outCount) {
  if (outCount) *outCount = 0;
  if (!path || !SdMan.exists(path)) return false;
  FsFile f;
  if (!SdMan.openFileForRead("TRA", path, f)) return false;
  uint32_t magic = 0, version = 0, cacheStart = 0, cacheCount = 0;
  uint8_t cachedEnc = 0;
  serialization::readPod(f, magic);
  serialization::readPod(f, version);
  serialization::readPod(f, cachedEnc);
  serialization::readPod(f, cacheStart);
  serialization::readPod(f, cacheCount);
  f.close();
  if (magic != kChapMagic || version != kChapVersion) return false;
  if (cachedEnc != expectEnc) return false;
  if (cacheStart != static_cast<uint32_t>(expectStart)) return false;
  if (cacheCount == 0 || cacheCount > 25) return false;
  if (outCount) *outCount = cacheCount;
  return true;
}

// Short batch is only valid at true EOF. If a later batch file already has
// chapters, this batch is truncated poison (mid-list blanks) → purge+rebuild.
bool isShortBatchWithLaterCache(const std::string& cachePath, uint8_t enc, int startChapter,
                                uint32_t count) {
  if (count >= 25) return false;
  char nextPath[128] = {0};
  chapterBatchPath(nextPath, sizeof(nextPath), cachePath, startChapter + 25);
  uint32_t nextCount = 0;
  return readChapterBatchHeader(nextPath, enc, startChapter + 25, &nextCount);
}
}  // namespace

bool Txt::hasChapterBatchCache(int startChapter) const {
  char loadPath[128] = {0};
  chapterBatchPath(loadPath, sizeof(loadPath), getCachePath(), startChapter);
  if (!SdMan.exists(loadPath)) return false;

  uint32_t cacheCount = 0;
  const uint8_t enc = static_cast<uint8_t>(encodingType);
  if (!readChapterBatchHeader(loadPath, enc, startChapter, &cacheCount)) {
    // Empty / wrong version / enc mismatch / corrupt header — delete so we rebuild.
    purgeChapterBatchFile(loadPath, "invalid_header");
    return false;
  }
  if (isShortBatchWithLaterCache(getCachePath(), enc, startChapter, cacheCount)) {
    purgeChapterBatchFile(loadPath, "short_batch_but_later_exists");
    return false;
  }
  return true;
}

bool Txt::parseChapterIndexAndOffset(int n, bool allowScan) {
    char readBuffer[128] = {0};
    int bufferLen = 0;

    // 配置参数
    const int CHAPTER_START = n;
    const int CHAPTER_END = n + 24;
    uint32_t VOLUME_PAGE_SIZE = 10240;  // dynamic, recalculated after fileSize is known
    const char* VOLUME_TITLE_PREFIX = "第";
    uint64_t CHAPTER_CHECK_THRESHOLD = VOLUME_PAGE_SIZE;
    const int MAX_BACK_SEARCH_LEN = 1024;
    // 向后探测的最大范围：覆盖下一批起始，避免无限扫描
    uint64_t MAX_NEXT_SEARCH = 2 * VOLUME_PAGE_SIZE;

    Serial.printf("[ChapterRange] ✅ 本次加载范围：%d ~ %d allowScan=%d\n", CHAPTER_START, CHAPTER_END,
                  allowScan ? 1 : 0);

    // Past verified EOF batch — never re-scan the whole 8MB file for empty ranges.
    if (m_emptyFromBatch_ >= 0 && n >= m_emptyFromBatch_) {
        chapterActualCount = 0;
        memset(chapterDataList, 0, sizeof(chapterDataList));
        Serial.printf("[ChapterLoader] ⛔ batch %d >= emptyFrom=%d, skip\n", n, m_emptyFromBatch_);
        return false;
    }

    // ========== 1. 优先读缓存（保持） ==========
    bool loadSuccess = loadChapterFromTxt(n);
    if (loadSuccess) {
        Serial.printf("[ChapterLoader] ✅ 缓存命中，直接返回\n");
        // Valid data past a previous empty marker means marker was wrong.
        if (m_emptyFromBatch_ >= 0 && n < m_emptyFromBatch_) {
            /* keep marker */
        } else if (chapterActualCount > 0 && m_emptyFromBatch_ >= 0 && n + 25 > m_emptyFromBatch_) {
            m_emptyFromBatch_ = -1;
        }
        return true;
    }

    if (!allowScan) {
        // Incomplete/poison cache was purged, or never built. Caller (TOC paint)
        // must not block the UI for a multi-second full-file scan.
        chapterActualCount = 0;
        memset(chapterDataList, 0, sizeof(chapterDataList));
        Serial.printf("[ChapterLoader] ⚠️ cache miss, skip scan (allowScan=0) batch=%d\n", n);
        return false;
    }

    // ========== 2. 初始化 + 获取文件总大小（保持） ==========
    chapterActualCount = 0;
    memset(chapterDataList, 0, sizeof(chapterDataList));
    uint64_t fileSize = 0;
    FsFile sizeFile;
    if (sizeFile.open(effectivePath.c_str(), FILE_READ)) {
        fileSize = sizeFile.size();
        sizeFile.close();
    } else {
        Serial.printf("[Parser] ❌ 无法获取文件大小，endOffset将设为0\n");
        return false;
    }

    // Dynamic VOLUME_PAGE_SIZE: guarantee <= 1000 chapters, minimum 10 KB
    {
      const uint32_t minPage = 10240;              // 10 KB floor
      const uint32_t needed = static_cast<uint32_t>((fileSize + 999) / 1000);
      VOLUME_PAGE_SIZE = (needed > minPage) ? needed : minPage;
      CHAPTER_CHECK_THRESHOLD = VOLUME_PAGE_SIZE;
      MAX_NEXT_SEARCH = 2ULL * VOLUME_PAGE_SIZE;
      Serial.printf("[Volume] Page size: %lu bytes (file %llu bytes)\n",
                    (unsigned long)VOLUME_PAGE_SIZE, (unsigned long long)fileSize);
    }

    // ========== 3. 分卷/章节模式检测 ==========
    // 大文件（>10MB）直接使用分卷模式，避免章节搜索导致内存不足崩溃
    constexpr uint64_t LARGE_FILE_THRESHOLD = 10ULL * 1024 * 1024; // 10MB
    if (!m_isVolumeOnlyBook && fileSize > LARGE_FILE_THRESHOLD) {
        Serial.printf("[VolumeMode] ⚠️ 文件 %llu 字节 > 10MB，强制使用分卷模式\n", (unsigned long long)fileSize);
        m_isVolumeOnlyBook = true;
    }

    if (!m_isVolumeOnlyBook) {
        FsFile checkFile;
        bool hasValidChapter = false;
        int chapterFoundCount = 0;

        // GBK conversion state for check file
        uint8_t checkGbkCarry = 0;
        bool checkGbkHasCarry = false;

        if (checkFile.open(effectivePath.c_str(), FILE_READ)) {
            Serial.printf("[Parser] ✅ 开始在 %lu 字节内检测是否有章节\n", VOLUME_PAGE_SIZE);
            bool skipBom = true;
            uint64_t currentReadOffset = 0;

            while (checkFile.available() && currentReadOffset < CHAPTER_CHECK_THRESHOLD) {
                bufferLen = 0;
                memset(readBuffer, 0, sizeof(readBuffer));

                while (checkFile.available() && currentReadOffset < CHAPTER_CHECK_THRESHOLD) {
                    char c = checkFile.read();
                    currentReadOffset++;
                    if (c == '\n' || c == '\r' || bufferLen >= 127) break;
                    readBuffer[bufferLen++] = c;
                }

                if (bufferLen == 0) continue;

                if (skipBom && bufferLen >= 3) {
                    if ((uint8_t)readBuffer[0] == 0xEF && (uint8_t)readBuffer[1] == 0xBB && (uint8_t)readBuffer[2] == 0xBF) {
                        memmove(readBuffer, readBuffer + 3, bufferLen - 3);
                        bufferLen -= 3;
                        skipBom = false;
                    }
                }

                // If GBK encoding, convert to UTF-8 before checking
                std::string convertedBuffer;
                const char* lineData = readBuffer;
                int lineLen = bufferLen;
                
                if (encodingType == TXT_ENCODING_GBK) {
                    convertedBuffer = convertGbkToUtf8(readBuffer, bufferLen, checkGbkCarry, checkGbkHasCarry);
                    lineData = convertedBuffer.c_str();
                    lineLen = convertedBuffer.size();
                }

                bool isChapter = (lineLen > 0 && lineLen <= 80) && isChapterLine(lineData, lineLen);
                if (isChapter) {
                    hasValidChapter = true;
                    chapterFoundCount++;
                    break;
                }
            }
            checkFile.close();
        } else {
            Serial.printf("[Parser] ❌ 打开文件失败，默认按分卷处理\n");
        }

        if (!hasValidChapter) {
            Serial.printf("[VolumeMode] ⚠️ %lu 字节内无章节，标记为纯分卷书籍\n", VOLUME_PAGE_SIZE);
            m_isVolumeOnlyBook = true;
        } else {
            Serial.printf("[ChapterMode] ✅ 检测到有效章节，走原章节解析逻辑\n");
        }
    }

    // ========== 4. 纯分卷模式（核心修改：向后探测下一分卷） ==========
    if (m_isVolumeOnlyBook) {
        FsFile file;
        if (!file.open(effectivePath.c_str(), FILE_READ)) {
            Serial.printf("[VolumeMode] ❌ 打开文件失败\n");
            goto save_and_exit;
        }

        int volCount = 0;
        uint64_t volOffsets[25] = {0}; // 存储当前批次偏移
        int volIndexes[25] = {0};      // 存储当前批次分卷号

        // 步骤1：解析当前批次25个分卷（保持）
        for (int i = 0; i < 25; ++i) {
            int volIdx = CHAPTER_START + i;
            uint64_t theoryOffset = (uint64_t)volIdx * VOLUME_PAGE_SIZE;
            uint64_t actualOffset = theoryOffset;

            if (volIdx > 0 && theoryOffset < fileSize) {
                if (file.seek(theoryOffset)) {
                    uint64_t backSearchStart = (theoryOffset >= MAX_BACK_SEARCH_LEN) ? (theoryOffset - MAX_BACK_SEARCH_LEN) : 0;
                    uint64_t currentSearchPos = theoryOffset;
                    bool foundNewLine = false;

                    // 1. 向后搜索 \n
                    while (currentSearchPos > backSearchStart) {
                        currentSearchPos--;
                        if (!file.seek(currentSearchPos)) break;
                        char c = file.read();
                        if (c == '\n') {
                            actualOffset = currentSearchPos + 1;
                            foundNewLine = true;
                            break;
                        }
                    }

                    // 2. 向后没找到，再向前搜索 \n（最多1024字节）
                    if (!foundNewLine && file.seek(theoryOffset)) {
                        uint64_t fwdLimit = std::min(theoryOffset + (uint64_t)MAX_BACK_SEARCH_LEN, (uint64_t)fileSize);
                        uint64_t fwdPos = theoryOffset;
                        while (fwdPos < fwdLimit) {
                            char c = file.read();
                            if (c == '\n') {
                                actualOffset = fwdPos + 1;
                                foundNewLine = true;
                                break;
                            }
                            fwdPos++;
                        }
                    }

                    // 3. 仍未找到 \n，对齐到 UTF-8 字符边界避免劈开多字节字符
                    if (!foundNewLine && file.seek(theoryOffset)) {
                        uint64_t pos = theoryOffset;
                        while (pos < fileSize) {
                            char c = file.read();
                            if ((static_cast<uint8_t>(c) & 0xC0) != 0x80) break; // 非续字节，即字符起始
                            pos++;
                        }
                        actualOffset = pos;
                    }

                    if (foundNewLine) {
                        Serial.printf("[Volume] ✅ 分卷%d 找到\\n，理论%llu → 实际%llu\n", volIdx, (unsigned long long)theoryOffset, (unsigned long long)actualOffset);
                    } else {
                        Serial.printf("[Volume] ⚠️ 分卷%d 未找到\\n，UTF-8对齐 理论%llu → 实际%llu\n", volIdx, (unsigned long long)theoryOffset, (unsigned long long)actualOffset);
                    }
                } else {
                    Serial.printf("[Volume] ❌ 分卷%d 定位失败，使用理论%llu\n", volIdx, (unsigned long long)theoryOffset);
                }
            }

            if (actualOffset >= fileSize) {
                if (volCount == 0) break;
                else continue;
            }

            volOffsets[volCount] = actualOffset;
            volIndexes[volCount] = volIdx;
            chapterDataList[volCount].chapterIndex = volIdx;
            chapterDataList[volCount].byteOffset = actualOffset;
            snprintf(chapterDataList[volCount].shortTitle, TITLE_BUF_SIZE - 1, "%s%d\xe8\x8a\x82", VOLUME_TITLE_PREFIX, volIdx + 1);
            chapterDataList[volCount].shortTitle[TITLE_BUF_SIZE - 1] = '\0';

            Serial.printf("[Volume] ✅ 分卷%d 已生成，实际偏移%llu\n", volIdx, (unsigned long long)actualOffset);
            volCount++;
        }

        // 步骤2：为每个分卷计算endOffset（核心：向后探测）
        for (int i = 0; i < volCount; i++) {
            if (i < volCount - 1) {
                // 非批次最后一个：用下一分卷的偏移
                chapterDataList[i].endOffset = volOffsets[i + 1];
            } else {
                // 批次最后一个：探测下一分卷（CHAPTER_START + volCount）
                int nextVolIdx = CHAPTER_START + volCount;
                uint64_t nextTheoryOffset = (uint64_t)nextVolIdx * VOLUME_PAGE_SIZE;
                uint64_t nextActualOffset = 0;
                bool hasNextVol = false;

                // 仅在理论偏移未超出文件且探测范围内时执行
                if (nextTheoryOffset < fileSize && nextTheoryOffset <= volOffsets[i] + MAX_NEXT_SEARCH) {
                    if (file.seek(nextTheoryOffset)) {
                        uint64_t backSearchStart = (nextTheoryOffset >= MAX_BACK_SEARCH_LEN) ? (nextTheoryOffset - MAX_BACK_SEARCH_LEN) : 0;
                        uint64_t currentSearchPos = nextTheoryOffset;
                        bool foundNewLine = false;

                        // 1. 向后搜索 \n
                        while (currentSearchPos > backSearchStart) {
                            currentSearchPos--;
                            if (!file.seek(currentSearchPos)) break;
                            char c = file.read();
                            if (c == '\n') {
                                nextActualOffset = currentSearchPos + 1;
                                foundNewLine = true;
                                break;
                            }
                        }

                        // 2. 向后没找到，再向前搜索 \n
                        if (!foundNewLine && file.seek(nextTheoryOffset)) {
                            uint64_t fwdLimit = std::min(nextTheoryOffset + (uint64_t)MAX_BACK_SEARCH_LEN, (uint64_t)fileSize);
                            uint64_t fwdPos = nextTheoryOffset;
                            while (fwdPos < fwdLimit) {
                                char c = file.read();
                                if (c == '\n') {
                                    nextActualOffset = fwdPos + 1;
                                    foundNewLine = true;
                                    break;
                                }
                                fwdPos++;
                            }
                        }

                        // 3. 仍未找到 \n，对齐到 UTF-8 字符边界
                        if (!foundNewLine && file.seek(nextTheoryOffset)) {
                            uint64_t pos = nextTheoryOffset;
                            while (pos < fileSize) {
                                char c = file.read();
                                if ((static_cast<uint8_t>(c) & 0xC0) != 0x80) break;
                                pos++;
                            }
                            nextActualOffset = pos;
                        }

                        // 验证下一分卷偏移有效性
                        if (nextActualOffset < fileSize && nextActualOffset > volOffsets[i]) {
                            hasNextVol = true;
                            Serial.printf("[Volume] ✅ 探测到下一分卷%d，偏移%llu\n", nextVolIdx, (unsigned long long)nextActualOffset);
                        }
                    }
                }

                // 赋值endOffset：有下一卷则用其偏移，否则用文件大小
                chapterDataList[i].endOffset = hasNextVol ? nextActualOffset : fileSize;
                Serial.printf("[Volume] ✅ 分卷%d endOffset：%llu（%s）\n",
                    volIndexes[i], (unsigned long long)chapterDataList[i].endOffset,
                    hasNextVol ? "下一分卷" : "文件末尾");
            }
        }

        file.close();
        chapterActualCount = volCount;
        goto save_and_exit;
    }

    // ========== 5. 有章节模式（核心修改：向后探测下一章节） ==========
    {
        FsFile file;
        if (!file.open(effectivePath.c_str(), FILE_READ)) {
            Serial.printf("[ChapterMode] ❌ 打开文件失败\n");
            goto save_and_exit;
        }

        const int MAX_VALID_LEN = 80;
        const int TITLE_SUB_LEN = 20;
        int chapterFoundCount = 0;
        int currSaveCount = 0;
        bool skipBom = true;
        uint64_t currentReadOffset = 0;
        uint64_t chapOffsets[25] = {0}; // 当前批次章节偏移
        int chapIndexes[25] = {0};      // 当前批次章节号

        // Resume optimization: if we're scanning sequentially (batch N follows batch N-1),
        // skip ahead to where the previous scan ended instead of re-reading from byte 0.
        // This turns O(B × F) into O(F) for the full conversion.
        if (CHAPTER_START > 0 && m_scanResumeChapterCount == CHAPTER_START && m_scanResumeOffset > 0) {
            currentReadOffset = m_scanResumeOffset;
            chapterFoundCount = m_scanResumeChapterCount;
            skipBom = false;
            if (!file.seek(currentReadOffset)) {
                Serial.printf("[ChapterMode] ⚠️ Resume seek failed, falling back to full scan\n");
                currentReadOffset = 0;
                chapterFoundCount = 0;
                skipBom = true;
                file.seek(0);
            } else {
                Serial.printf("[ChapterMode] ✅ Resuming scan from offset %llu, chapter %d\n",
                    (unsigned long long)currentReadOffset, chapterFoundCount);
            }
        } else if (CHAPTER_START == 0) {
            // Reset resume state when starting a fresh scan from batch 0
            m_scanResumeOffset = 0;
            m_scanResumeChapterCount = 0;
        }

        // Copy up to keepCount UTF-8 code points into dst (TITLE_BUF_SIZE).
        // Must not split multi-byte sequences (invalid UTF-8 → '?' glyphs in UI).
        auto subUTF8String = [](char* dst, const char* src, int len, int keepCount) {
            int charCount = 0, i = 0, o = 0;
            memset(dst, 0, TITLE_BUF_SIZE);
            while (i < len && charCount < keepCount && o + 1 < TITLE_BUF_SIZE) {
                const uint8_t c = static_cast<uint8_t>(src[i]);
                int clen = 1;
                if (c >= 0xF0) clen = 4;
                else if (c >= 0xE0) clen = 3;
                else if (c >= 0xC0) clen = 2;
                if (i + clen > len) break;  // truncated unit — stop rather than emit junk
                if (o + clen >= TITLE_BUF_SIZE) break;
                memcpy(dst + o, src + i, static_cast<size_t>(clen));
                o += clen;
                i += clen;
                charCount++;
            }
            dst[o] = '\0';
        };

        // 缓冲读取器：避免逐字节 file.read() 的 SD 卡 I/O 开销，使用 512 字节批量读取提升性能
        uint8_t ioChunkBuf[512];
        int ioChunkFilled = 0, ioChunkPos = 0;
        bool ioEof = false;
        auto readNextChar = [&](char& c) -> bool {
            if (ioChunkPos >= ioChunkFilled) {
                ioChunkFilled = (int)file.read(ioChunkBuf, sizeof(ioChunkBuf));
                ioChunkPos = 0;
                if (ioChunkFilled <= 0) { ioEof = true; return false; }
            }
            c = (char)ioChunkBuf[ioChunkPos++];
            currentReadOffset++;
            return true;
        };

        // GBK conversion state for chapter title extraction
        uint8_t gbkCarry = 0;
        bool gbkHasCarry = false;
        std::string gbkPendingBuffer;  // Store pending GBK bytes for conversion

        // 步骤1：解析当前批次25个章节（保持）
        while (!ioEof && currSaveCount < 25) {
            bufferLen = 0;
            memset(readBuffer, 0, sizeof(readBuffer));

            while (true) {
                char c;
                if (!readNextChar(c)) break;
                if (c == '\n' || c == '\r' || bufferLen >= 127) break;
                readBuffer[bufferLen++] = c;
            }

            if (bufferLen == 0) continue;

            // Skip UTF-8 BOM if present
            if (skipBom && bufferLen >= 3) {
                if ((uint8_t)readBuffer[0] == 0xEF && (uint8_t)readBuffer[1] == 0xBB && (uint8_t)readBuffer[2] == 0xBF) {
                    memmove(readBuffer, readBuffer + 3, bufferLen - 3);
                    bufferLen -= 3;
                    skipBom = false;
                }
            }

            // If GBK encoding, convert to UTF-8 before processing
            std::string convertedLine;
            const char* lineData = readBuffer;
            int lineLen = bufferLen;
            
            if (encodingType == TXT_ENCODING_GBK) {
                // Convert GBK to UTF-8
                convertedLine = convertGbkToUtf8(readBuffer, bufferLen, gbkCarry, gbkHasCarry);
                lineData = convertedLine.c_str();
                lineLen = convertedLine.size();
            }

            bool isChapter = (lineLen > 0 && lineLen <= MAX_VALID_LEN) && isChapterLine(lineData, lineLen);
            if (isChapter) {
                if (chapterFoundCount >= CHAPTER_START && chapterFoundCount <= CHAPTER_END) {
                    uint64_t pos = currentReadOffset - bufferLen - 1;
                    if (pos < 0) pos = 0;

                    chapOffsets[currSaveCount] = pos;
                    chapIndexes[currSaveCount] = chapterFoundCount;
                    chapterDataList[currSaveCount].chapterIndex = chapterFoundCount;
                    chapterDataList[currSaveCount].byteOffset = pos;
                    
                    // Extract title from converted UTF-8 line
                    subUTF8String(chapterDataList[currSaveCount].shortTitle, lineData, lineLen, TITLE_SUB_LEN);
                    currSaveCount++;
                }
                chapterFoundCount++;
            }
        }

        // 步骤2：为每个章节计算endOffset（核心：向后探测）
        for (int i = 0; i < currSaveCount; i++) {
            if (i < currSaveCount - 1) {
                // 非批次最后一个：用下一章节的偏移
                chapterDataList[i].endOffset = chapOffsets[i + 1];
            } else {
                // 批次最后一个：探测下一章节（chapterFoundCount）
                uint64_t searchStart = chapOffsets[i] + 1;
                uint64_t searchEnd = searchStart + MAX_NEXT_SEARCH;
                if (searchEnd > fileSize) searchEnd = fileSize;
                uint64_t nextChapOffset = 0;
                bool hasNextChap = false;

                // GBK conversion state for inner buffer
                uint8_t innerGbkCarry = gbkCarry;
                bool innerGbkHasCarry = gbkHasCarry;

                // 仅在搜索范围有效时执行
                if (searchStart < fileSize) {
                    if (file.seek(searchStart)) {
                        bool innerSkipBom = false; // 内部BOM已在主解析中处理
                        uint64_t innerReadOffset = searchStart;
                        char innerBuffer[128] = {0};
                        int innerBufLen = 0;

                        while (file.available() && innerReadOffset < searchEnd) {
                            innerBufLen = 0;
                            memset(innerBuffer, 0, sizeof(innerBuffer));

                            while (file.available() && innerReadOffset < searchEnd) {
                                char c = file.read();
                                innerReadOffset++;
                                if (c == '\n' || c == '\r' || innerBufLen >= 127) break;
                                innerBuffer[innerBufLen++] = c;
                            }

                            if (innerBufLen == 0) continue;

                            // If GBK encoding, convert to UTF-8 before checking
                            std::string convertedInnerBuffer;
                            const char* innerLineData = innerBuffer;
                            int innerLineLen = innerBufLen;
                            
                            if (encodingType == TXT_ENCODING_GBK) {
                                convertedInnerBuffer = convertGbkToUtf8(innerBuffer, innerBufLen, innerGbkCarry, innerGbkHasCarry);
                                innerLineData = convertedInnerBuffer.c_str();
                                innerLineLen = convertedInnerBuffer.size();
                            }

                            bool isNextChapter = (innerLineLen > 0 && innerLineLen <= MAX_VALID_LEN) && isChapterLine(innerLineData, innerLineLen);
                            if (isNextChapter) {
                                // 计算下一章节的起始偏移
                                nextChapOffset = innerReadOffset - innerBufLen - 1;
                                if (nextChapOffset < 0) nextChapOffset = 0;
                                if (nextChapOffset > chapOffsets[i] && nextChapOffset < fileSize) {
                                    hasNextChap = true;
                                    Serial.printf("[Chapter] ✅ 探测到下一章节%d，偏移%llu\n", chapterFoundCount, (unsigned long long)nextChapOffset);
                                    break; // 找到即退出，避免多余扫描
                                }
                            }
                        }
                        memset(innerBuffer, 0, sizeof(innerBuffer)); // 清理临时缓冲区
                    }
                }

                // 赋值endOffset：有下一章节则用其偏移，否则用文件大小
                chapterDataList[i].endOffset = hasNextChap ? nextChapOffset : fileSize;
                Serial.printf("[Chapter] ✅ 章节%d endOffset：%llu（%s）\n",
                    chapIndexes[i], (unsigned long long)chapterDataList[i].endOffset,
                    hasNextChap ? "下一章节" : "文件末尾");
            }
        }

        file.close();
        chapterActualCount = currSaveCount;

        // Save resume state for next sequential batch call
        m_scanResumeOffset = currentReadOffset;
        m_scanResumeChapterCount = chapterFoundCount;
    }

    // ========== 6. 保存缓存并退出 ==========
    // Only persist non-empty batches. An empty chapters_N_25.bin is treated as a
    // cache miss on load (see loadChapterFromTxt) so a failed/partial scan cannot
    // permanently blank a mid-book range.
save_and_exit:
    if (chapterActualCount > 0) {
        Serial.printf("[Result] ✅ 本次生成 %d 个有效条目，endOffset已按文件实际末尾校准\n", chapterActualCount);
        saveChapterToTxt(n);
        // Short batch after a full scan ⇒ EOF: no more chapters beyond this range.
        if (chapterActualCount < 25) {
            m_emptyFromBatch_ = n + 25;
            Serial.printf("[ChapterLoader] EOF after batch %d (got %d < 25), emptyFrom=%d\n", n,
                          chapterActualCount, m_emptyFromBatch_);
        }
    } else {
        Serial.printf("[Result] ⚠️ 本次无有效条目，跳过写缓存（避免空批次永久占坑）\n");
        // Full scan found nothing in this batch — remember so +100 jumps past end
        // do not re-scan the entire file for 10–30s each time (UI freeze).
        if (m_emptyFromBatch_ < 0 || n < m_emptyFromBatch_) {
            m_emptyFromBatch_ = n;
        }
        Serial.printf("[ChapterLoader] mark emptyFrom batch=%d\n", m_emptyFromBatch_);
    }
    memset(readBuffer, 0, sizeof(readBuffer));
    return false;  // built by scan (not a warm cache hit)
}


// 保存25章到单个TXT（纯C风格，无String）
// 先确保必要的宏/类型定义（如果未定义）
#ifndef CACHE_MAGIC
#define CACHE_MAGIC 0x43484150  // "CHAP" ASCII码，自定义魔数
#endif

#ifndef CACHE_VERSION
// v4: raw-source byte offsets + encodingType in header (post multipoint / stream decode)
#define CACHE_VERSION 4
#endif

// 保存25章到单个BIN文件（使用serialization::writePod/writeString规范）
void Txt::saveChapterToTxt(int startChapter) {
    FsFile f;
    char savePath[128] = {0};
    // 文件名格式：chapters_起始章n_25.bin
    snprintf(savePath, sizeof(savePath), "%s/chapters_%d_25.bin", getCachePath().c_str(), startChapter);

    // 打开文件（失败则直接返回并打印日志）
    if (!SdMan.openFileForWrite("TRA", savePath, f)) {
        Serial.printf("[ChapterSaver] ❌ %d~%d章合并保存失败 → %s\n", 
                      startChapter, startChapter+24, savePath);
        return;
    }

    // ========== 1. 写入缓存头部（和index.bin格式保持一致） ==========
    serialization::writePod(f, CACHE_MAGIC);                // 魔数（验证文件合法性）
    serialization::writePod(f, static_cast<uint32_t>(CACHE_VERSION));  // v4 raw offsets + enc
    serialization::writePod(f, static_cast<uint8_t>(encodingType));   // encoding-aware offsets
    serialization::writePod(f, static_cast<uint32_t>(startChapter));  // 起始章节号
    serialization::writePod(f, static_cast<uint32_t>(chapterActualCount));  // 实际保存章节数

    // ========== 2. 写入章节数据主体（使用writeString存储标题） ==========
    for (int i = 0; i < chapterActualCount && i < 25; i++) {
        // 1. 章节序号（int → int32_t 保证长度统一）
        serialization::writePod(f, static_cast<int32_t>(chapterDataList[i].chapterIndex));
        // 2. 字节偏移量（uint32_t 直接写入）
        serialization::writePod(f, chapterDataList[i].byteOffset);
        // 3. 短标题：char数组 → 用writeString序列化（自动处理长度+内容）
        // 核心调整：替换writePod为writeString，适配字符串存储规范
        serialization::writeString(f, chapterDataList[i].shortTitle);
        // 4. 章节结束偏移（uint32_t 直接写入）
        serialization::writePod(f, chapterDataList[i].endOffset);
    }

    // ========== 3. 完成写入 ==========
    f.sync();  // 同步到磁盘，防止数据丢失
    f.close();

    Serial.printf("[ChapterSaver] ✅ %d~%d章合并保存成功 → %s | 实际保存%d章 | 魔数：0x%X 版本：%d\n", 
                  startChapter, startChapter+24, savePath, chapterActualCount, CACHE_MAGIC, CACHE_VERSION);
}

// 加载25章从单个TXT（纯C风格，无String）
bool Txt::loadChapterFromTxt(int startChapter) {
    // ========== 1. 初始化/清理数据（保留原loadChapterFromTxt的清理逻辑） ==========
    chapterActualCount = 0;
    memset(chapterDataList, 0, sizeof(chapterDataList));
    bool loadOk = false;

    FsFile f;
    char loadPath[128] = {0};
    chapterBatchPath(loadPath, sizeof(loadPath), getCachePath(), startChapter);
    const uint8_t enc = static_cast<uint8_t>(encodingType);

    // 打开文件失败（对齐参考示例的日志风格）
    if (!SdMan.openFileForRead("TRA", loadPath, f)) {
        Serial.printf("[%lu] [TRA] No chapter cache found for %d~%d → %s\n", millis(), startChapter, startChapter+24, loadPath);
        return false;
    }

    // ========== 2. 读取并验证头部（完全对齐loadPageIndexCache风格） ==========
    // 2.1 读取魔数并验证
    uint32_t magic;
    serialization::readPod(f, magic);
    if (magic != CACHE_MAGIC) {
        Serial.printf("[%lu] [TRA] Chapter cache magic mismatch (0x%X != 0x%X), rebuilding\n", 
                      millis(), magic, CACHE_MAGIC);
        f.close();
        purgeChapterBatchFile(loadPath, "magic_mismatch");
        return false;
    }

    // 2.2 读取版本号并验证
    uint32_t version; // 对齐参考示例用uint32_t，若原版本是uint8_t可调整
    serialization::readPod(f, version);
    if (version != CACHE_VERSION) {
        Serial.printf("[%lu] [TRA] Chapter cache version mismatch (%d != %d), rebuilding\n", 
                      millis(), version, CACHE_VERSION);
        f.close();
        purgeChapterBatchFile(loadPath, "version_mismatch");
        return false;
    }

    // 2.2b encodingType — offsets are raw-source encoding-aware
    uint8_t cachedEnc = 0;
    serialization::readPod(f, cachedEnc);
    if (cachedEnc != enc) {
        Serial.printf("[%lu] [TRA] Chapter cache encoding mismatch (%u != %d), rebuilding\n", millis(),
                      static_cast<unsigned>(cachedEnc), encodingType);
        f.close();
        purgeChapterBatchFile(loadPath, "encoding_mismatch");
        return false;
    }

    // 2.3 读取起始章号并验证（确保缓存文件和要加载的章节匹配）
    uint32_t cacheStartChapter;
    serialization::readPod(f, cacheStartChapter);
    if (cacheStartChapter != static_cast<uint32_t>(startChapter)) {
        Serial.printf("[%lu] [TRA] Chapter cache start mismatch (%d != %d), rebuilding\n", 
                      millis(), cacheStartChapter, startChapter);
        f.close();
        purgeChapterBatchFile(loadPath, "start_mismatch");
        return false;
    }

    // 2.4 读取缓存的章节总数
    uint32_t cacheChapterCount;
    serialization::readPod(f, cacheChapterCount);
    // Empty / oversize / short-with-later-batch are poison mid-list blanks.
    if (cacheChapterCount == 0 || cacheChapterCount > 25) {
        Serial.printf("[%lu] [TRA] Chapter cache count invalid (%u), rebuilding\n",
                      millis(), static_cast<unsigned>(cacheChapterCount));
        f.close();
        purgeChapterBatchFile(loadPath, "count_invalid");
        return false;
    }
    if (isShortBatchWithLaterCache(getCachePath(), enc, startChapter, cacheChapterCount)) {
        Serial.printf("[%lu] [TRA] Short batch %d count=%u but later batch exists — purge+rebuild\n",
                      millis(), startChapter, static_cast<unsigned>(cacheChapterCount));
        f.close();
        purgeChapterBatchFile(loadPath, "short_batch_but_later_exists");
        return false;
    }

    // ========== 3. 读取章节数据主体（逐字段+验证） ==========
     int chapterNum = 0;
    while (chapterNum < 25 && chapterNum < static_cast<int>(cacheChapterCount) && f.available()) {
        // 3.1 读取章节序号
        int32_t actualChap;
        serialization::readPod(f, actualChap);

        // 3.2 读取字节偏移量
        uint32_t byteOffset;
        serialization::readPod(f, byteOffset);

        // 3.3 读取短标题：核心调整为std::string类型
        std::string titleStr; // 必须使用std::string
        serialization::readString(f, titleStr); // 直接读取到string，无需缓冲区

        // 3.4 读取结束偏移量
        uint32_t endOffset;
        serialization::readPod(f, endOffset);

        // ========== 4. 填充数据（string转char数组，保证结构体兼容） ==========
        chapterDataList[chapterNum].chapterIndex = actualChap;
        chapterDataList[chapterNum].byteOffset = byteOffset;
        chapterDataList[chapterNum].endOffset = endOffset;

        // 清空标题数组 + string安全拷贝到char数组（防止越界）
        memset(chapterDataList[chapterNum].shortTitle, 0, TITLE_BUF_SIZE);
        strncpy(chapterDataList[chapterNum].shortTitle, titleStr.c_str(), TITLE_BUF_SIZE - 1);

        // 清理string（可选，加速内存释放）
        titleStr.clear();
        titleStr.shrink_to_fit();

        chapterNum++;
        loadOk = true;
    }

    // ========== 5. 收尾处理（对齐参考示例） ==========
    f.close();

    // Truncated body (header promised more than we could read) → purge.
    if (loadOk && static_cast<uint32_t>(chapterNum) != cacheChapterCount) {
        Serial.printf("[%lu] [TRA] Chapter cache body short (%d < %u), purge+rebuild\n", millis(),
                      chapterNum, static_cast<unsigned>(cacheChapterCount));
        chapterActualCount = 0;
        memset(chapterDataList, 0, sizeof(chapterDataList));
        purgeChapterBatchFile(loadPath, "body_truncated");
        return false;
    }

    chapterActualCount = chapterNum;

    // 日志输出（融合参考示例+业务逻辑）
    if (loadOk) {
        Serial.printf("[%lu] [TRA] Loaded chapter cache: %d~%d → %s | %d chapters\n", 
                      millis(), startChapter, startChapter+24, loadPath, chapterActualCount);
    }

    return loadOk;
}