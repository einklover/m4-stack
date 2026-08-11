#pragma once

#include <SDCardManager.h>

#include <cstdint>
#include <memory>
#include <string>

// Encoding type constants (legacy; prefer getTxtEnc())
#define TXT_ENCODING_UNKNOWN 0
#define TXT_ENCODING_UTF8    1
#define TXT_ENCODING_GBK     2
#define TXT_ENCODING_UTF16LE 3
#define TXT_ENCODING_UTF16BE 4

// 宏定义常量 
#define MAX_SAVE_CHAPTER  30    // 最多存30章
#define TITLE_KEEP_LENGTH 20    // 标题截取前20个UTF8字符
#define TITLE_BUF_SIZE    64    // 标题缓冲区64字节，完美匹配你的static char title[64]
#define MAX_SAVE_PAGE 100

// 目录结构体
struct ChapterData {
    int chapterIndex;        // 章节序号
    uint32_t byteOffset;     // 字节偏移量（原始文件字节）
    char shortTitle[TITLE_BUF_SIZE]; // 截取后的标题，char数组格式
    uint32_t endOffset;      // 章节结束的字节偏移（可选，预留字段）
};

class Txt {
  std::string filepath;       // 原始文件路径
  std::string effectivePath;  // 实际读取路径（direct path: always original file）
  std::string cacheBasePath;
  std::string cachePath;
  bool loaded = false;
  size_t fileSize = 0;
  size_t contentBomSkip = 0;  // skip UTF-8/UTF-16 BOM for content reads
  
  // Encoding detection (see TXT_ENCODING_*)
  mutable int encodingType = TXT_ENCODING_UNKNOWN;
  // Non-empty when detection is weak / unsupported (e.g. gb18030_4byte_unsupported)
  mutable std::string encodingDiagnostic;
  
  // Legacy whole-file GBK→UTF-8 cache (NOT used on direct-read path; kept for
  // optional tooling / old caches that must not be deleted).
  bool convertGbkToUtf8Cache();
  std::string getUtf8CachePath() const;

  //加目录

  ChapterData chapterDataList[MAX_SAVE_CHAPTER];
  int chapterActualCount = 0;
  void splitChaptersByNewline(); 
  void saveChapterToTxt(int startChapter);
  bool loadChapterFromTxt(int startChapter);
  
  bool m_isVolumeOnlyBook = false;

  // Chapter-mode scan resume state: avoids O(n²) re-scanning from byte 0
  // when batches are requested sequentially (e.g. 0, 25, 50, ...).
  uint64_t m_scanResumeOffset = 0;    // file offset to resume scanning from
  int m_scanResumeChapterCount = 0;   // chapters found up to that offset
  // After a full scan finds no chapters in batch N (EOF past last chapter),
  // refuse further scans for N, N+25, ... (skip+100 past end was multi-10s freeze).
  int m_emptyFromBatch_ = -1;



 public:
  explicit Txt(std::string path, std::string cacheBasePath);

  bool load();
  [[nodiscard]] const std::string& getPath() const { return filepath; }
  [[nodiscard]] const std::string& getCachePath() const { return cachePath; }
  [[nodiscard]] std::string getTitle() const;
  [[nodiscard]] size_t getFileSize() const { return fileSize; }
  [[nodiscard]] bool isVolumeOnlyBook() const { return m_isVolumeOnlyBook; }

  void setupCacheDir() const;

  // Cover image support - looks for cover.bmp/jpg/jpeg/png in same folder as txt file
  [[nodiscard]] std::string getCoverBmpPath() const;
  [[nodiscard]] bool generateCoverBmp() const;
  [[nodiscard]] std::string findCoverImage() const;

  // Read raw (or optionally stream-decoded UTF-8) content. Offsets are always in
  // original file bytes. convertToUtf8 only rewrites the buffer contents; length
  // is still raw-read size unless actualLen reports decoded size when converting.
  [[nodiscard]] bool readContent(uint8_t* buffer, size_t offset, size_t length, 
                                  bool convertToUtf8 = true, size_t* actualLen = nullptr) const;
  // True when source is GBK dual-byte (stream-decode whole window in reader).
  [[nodiscard]] bool isGbkEncoding() const { return encodingType == TXT_ENCODING_GBK; }
  [[nodiscard]] bool isUtf16Encoding() const {
    return encodingType == TXT_ENCODING_UTF16LE || encodingType == TXT_ENCODING_UTF16BE;
  }
  // False for Unknown / unsupported (e.g. GB18030 4-byte) — do not open as UTF-8 garbage.
  [[nodiscard]] bool isEncodingSupported() const {
    return encodingType == TXT_ENCODING_UTF8 || encodingType == TXT_ENCODING_GBK ||
           encodingType == TXT_ENCODING_UTF16LE || encodingType == TXT_ENCODING_UTF16BE;
  }
  [[nodiscard]] int getEncodingType() const { return encodingType; }
  [[nodiscard]] const char* getEncodingName() const;
  [[nodiscard]] const char* getEncodingDiagnostic() const {
    return encodingDiagnostic.empty() ? "" : encodingDiagnostic.c_str();
  }
  [[nodiscard]] size_t getContentBomSkip() const { return contentBomSkip; }
  //加目录

  uint32_t getChapterOffsetByIndex(int chapterIndex) {
      for(int i = 0; i < chapterActualCount; i++) {
          if(chapterDataList[i].chapterIndex == chapterIndex) {
              return chapterDataList[i].byteOffset;
          }
      }
      return 0; // 无此章节返回0
  }
uint32_t getChapterendOffsetByIndex(int chapterIndex) {
      for(int i = 0; i < chapterActualCount; i++) {
          if(chapterDataList[i].chapterIndex == chapterIndex) {
              return chapterDataList[i].endOffset;
          }
      }
      return 0; // 无此章节返回0
  }
  std::string getChapterTitleByIndex(int chapterIndex) {
      for(int i = 0; i < chapterActualCount; i++) {
          if(chapterDataList[i].chapterIndex == chapterIndex) {
              return std::string(chapterDataList[i].shortTitle);
          }
      }
      return ""; // 无此章节返回空字符串
  }

  // Returns a direct pointer to the title buffer (no heap allocation).
  // Returns nullptr if chapter not found.
  const char* getChapterTitlePtr(int chapterIndex) {
      for(int i = 0; i < chapterActualCount; i++) {
          if(chapterDataList[i].chapterIndex == chapterIndex) {
              return chapterDataList[i].shortTitle;
          }
      }
      return nullptr;
  }

  // ✅ 补充章节存在判断接口（和上面配套，之前漏掉了，已补全）
  bool isChapterExist(int chapterIndex) {
      for(int i = 0; i < chapterActualCount; i++) {
          if(chapterDataList[i].chapterIndex == chapterIndex) {
              return true;
          }
      }
      return false;
  }

  // True if SD already has a valid chapters_<n>_25.bin for this encoding (no scan).
  bool hasChapterBatchCache(int startChapter) const;

  // Load batch starting at n (multiples of 25).
  // Returns true if served from SD cache. If allowScan is false and cache misses,
  // returns false without a multi-second file scan (UI must not block on rebuild).
  bool parseChapterIndexAndOffset(int n, bool allowScan = true);

};
