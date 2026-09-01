#pragma once
#include <Print.h>

#include <algorithm>
#include <vector>

#include "Epub.h"
#include "expat.h"

class BookMetadataCache;

class ContentOpfParser final : public Print {
  enum ParserState {
    START,
    IN_PACKAGE,
    IN_METADATA,
    IN_BOOK_TITLE,
    IN_BOOK_AUTHOR,
    IN_BOOK_LANGUAGE,
    IN_MANIFEST,
    IN_SPINE,
    IN_GUIDE,
  };

  const std::string& cachePath;
  const std::string& baseContentPath;
  size_t remainingSize;
  XML_Parser parser = nullptr;
  ParserState state = START;
  BookMetadataCache* cache;
  FsFile tempItemStore;
  char coverItemId[128] = {};

  // Fixed-size binary index stored on SD card (avoids heap allocation for large manifests)
  struct ItemIndexEntry {
    uint32_t idHash;      // FNV-1a hash of itemId
    uint16_t idLen;       // length for collision reduction
    uint32_t fileOffset;  // offset in .items.bin
  };
  static constexpr char indexCacheFile[] = "/.index.bin";
  FsFile indexStore;             // Stores ItemIndexEntry structs (binary, fixed-size per entry)
  uint32_t indexEntryCount = 0;  // Number of entries written
  bool indexSorted = false;      // (unused; kept for future SD fallback)
  ItemIndexEntry* ramIndex = nullptr;  // Malloc'd sorted array for O(log n) in-RAM lookup

  static constexpr uint16_t LARGE_SPINE_THRESHOLD = 400;

  // FNV-1a hash from C string (no heap allocation)
  static uint32_t fnvHashCStr(const char* s) {
    uint32_t hash = 2166136261u;
    while (*s) {
      hash ^= static_cast<uint8_t>(*s++);
      hash *= 16777619u;
    }
    return hash;
  }
  // FNV-1a hash from std::string (kept for compatibility)
  static uint32_t fnvHash(const std::string& s) { return fnvHashCStr(s.c_str()); }

  // Stack-based path normaliser: concatenates base+rel and resolves '..' segments.
  // No heap allocation.
  static void normalisePathInto(char* out, size_t outSize, const char* base, size_t baseLen,
                                const char* rel);
  // Write a null-terminated C-string to FsFile with a uint32_t length prefix.
  static void writeStringChar(FsFile& file, const char* s);
  // Read a length-prefixed string from FsFile into a char buffer; always null-terminates.
  static bool readStringToChar(FsFile& file, char* buf, size_t bufSize);
  // Skip a length-prefixed string in FsFile.
  static bool skipString(FsFile& file);

  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);
  static void XMLCALL endElement(void* userData, const XML_Char* name);

 public:
  std::string title;
  std::string author;
  std::string language;
  std::string tocNcxPath;
  std::string tocNavPath;  // EPUB 3 nav document path
  std::string coverItemHref;
  std::string textReferenceHref;
  std::vector<std::string> cssFiles;  // CSS stylesheet paths

  explicit ContentOpfParser(const std::string& cachePath, const std::string& baseContentPath, const size_t xmlSize,
                            BookMetadataCache* cache)
      : cachePath(cachePath), baseContentPath(baseContentPath), remainingSize(xmlSize), cache(cache) {}
  ~ContentOpfParser() override;

  bool setup();

  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;
};
