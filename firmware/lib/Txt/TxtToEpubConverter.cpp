#include "TxtToEpubConverter.h"

#include <HardwareSerial.h>
#include <SDCardManager.h>
#include <Serialization.h>

#include <algorithm>
#include <string>
#include <vector>

#include "GbkToUtf8.h"
#include "MinimalZipWriter.h"
#include "Txt.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

constexpr size_t READ_CHUNK = 4096;  // 4 KB: reduces SD read calls ~8× vs 512 B
constexpr uint32_t EPUB_CONVERSION_VERSION = 5;  // Bump to invalidate cached EPUBs after code changes

// Chapter metadata collected during the chapter-scan phase.
// Compact: only offsets, no title strings. Titles are re-derived from the
// Txt object on demand (loaded 25 at a time from cache) to keep peak heap
// usage bounded even for books with thousands of chapters/volumes.
struct ChapterMeta {
  int index;
  uint32_t begin;
  uint32_t end;
};

// Derive the same cache-path that Txt uses: {cacheDir}/txt_{hash}
std::string txtCachePath(const std::string& txtPath, const std::string& cacheDir) {
  const size_t hash = std::hash<std::string>{}(txtPath);
  return cacheDir + "/txt_" + std::to_string(hash);
}

// Escape XML special characters (&, <, >) in a raw byte buffer.
// Also strips characters that are illegal in XML 1.0:
//   - Control chars 0x00-0x08, 0x0B, 0x0C, 0x0E-0x1F, and 0x7F
//   - Invalid or truncated UTF-8 byte sequences (e.g. stray GBK bytes)
//   - Unicode code points illegal in XML 1.0: surrogates (U+D800-DFFF),
//     noncharacters (U+FFFE, U+FFFF), and code points above U+10FFFF.
// Legal XML 1.0 chars: #x9 | #xA | #xD | [#x20-#xD7FF] | [#xE000-#xFFFD] | [#x10000-#x10FFFF]
// (0x09=tab, 0x0A=LF, 0x0D=CR are the only allowed controls in XML 1.0)
std::string xmlEscape(const char* buf, size_t len) {
  std::string out;
  out.reserve(len + 16);
  for (size_t i = 0; i < len; ) {
    const unsigned char c = static_cast<unsigned char>(buf[i]);

    // --- ASCII range ---
    if (c < 0x80) {
      i++;
      // Skip XML 1.0 illegal control characters
      if (c < 0x20 && c != 0x09 && c != 0x0A && c != 0x0D) continue;
      if (c == 0x7F) continue;
      if (c == '&')       out += "&amp;";
      else if (c == '<')  out += "&lt;";
      else if (c == '>')  out += "&gt;";
      else                out += static_cast<char>(c);
      continue;
    }

    // --- Multi-byte UTF-8 sequence ---
    // Determine expected sequence length from leading byte.
    // 0xC0/0xC1 are overlong and invalid; 0xF5-0xFF exceed Unicode range.
    int seqLen = 0;
    if      (c >= 0xC2 && c <= 0xDF) seqLen = 2;
    else if (c >= 0xE0 && c <= 0xEF) seqLen = 3;
    else if (c >= 0xF0 && c <= 0xF4) seqLen = 4;
    else { i++; continue; }  // Invalid leading byte – skip it

    // Check all continuation bytes are present and valid (0x80-0xBF)
    if (i + static_cast<size_t>(seqLen) > len) break;  // Truncated – stop here, caller keeps remainder
    bool valid = true;
    for (int j = 1; j < seqLen; j++) {
      if ((static_cast<unsigned char>(buf[i + j]) & 0xC0) != 0x80) {
        valid = false;
        break;
      }
    }
    if (!valid) { i++; continue; }  // Bad continuation byte – skip leader

    // Decode Unicode code point and validate against XML 1.0 legal ranges
    uint32_t cp = 0;
    if (seqLen == 2) {
      cp = (static_cast<uint32_t>(c & 0x1F) << 6)
         |  static_cast<uint32_t>(static_cast<unsigned char>(buf[i+1]) & 0x3F);
    } else if (seqLen == 3) {
      cp = (static_cast<uint32_t>(c & 0x0F) << 12)
         | (static_cast<uint32_t>(static_cast<unsigned char>(buf[i+1]) & 0x3F) << 6)
         |  static_cast<uint32_t>(static_cast<unsigned char>(buf[i+2]) & 0x3F);
    } else {  // seqLen == 4
      cp = (static_cast<uint32_t>(c & 0x07) << 18)
         | (static_cast<uint32_t>(static_cast<unsigned char>(buf[i+1]) & 0x3F) << 12)
         | (static_cast<uint32_t>(static_cast<unsigned char>(buf[i+2]) & 0x3F) << 6)
         |  static_cast<uint32_t>(static_cast<unsigned char>(buf[i+3]) & 0x3F);
    }

    // XML 1.0 legal: [#x20-#xD7FF] | [#xE000-#xFFFD] | [#x10000-#x10FFFF]
    // Drop surrogates (D800-DFFF), noncharacters (FFFE/FFFF), and out-of-range.
    bool xmlLegal = (cp >= 0x20 && cp <= 0xD7FF)
                 || (cp >= 0xE000 && cp <= 0xFFFD)
                 || (cp >= 0x10000 && cp <= 0x10FFFF);
    if (!xmlLegal) { i += static_cast<size_t>(seqLen); continue; }  // Skip illegal code point

    // Copy the valid UTF-8 sequence as-is
    for (int j = 0; j < seqLen; j++) out += buf[i + j];
    i += static_cast<size_t>(seqLen);
  }
  return out;
}

// Helper macro: write a string-literal chunk to the streaming ZIP entry.
#define WC(zip, s) (zip).writeFileChunk(reinterpret_cast<const uint8_t*>(s), strlen(s))

// Stream one chapter's XHTML content directly into a begun ZIP entry.
// txtFile must already be open; this function seeks to [begin, end) within it.
// Keeping the file handle open across chapters avoids N file-open/close pairs.
// Call zip.beginFileEntry() before and zip.endFileEntry() after.
static void writeChapterXhtml(MinimalZipWriter& zip, const char* title,
                               FsFile& txtFile, uint32_t begin, uint32_t end,
                               const uint16_t* gbkTable, bool titleAlreadyUtf8) {
  // If GBK, convert the title to UTF-8 first – but skip for programmatically
  // generated titles (e.g. volume titles "第N节") which are always UTF-8.
  std::string titleUtf8;
  const char* titleSrc = title;
  if (gbkTable && !titleAlreadyUtf8) {
    uint8_t carry = 0; bool hasCarry = false;
    titleUtf8 = gbkChunkToUtf8(reinterpret_cast<const uint8_t*>(title),
                                strlen(title), gbkTable, carry, hasCarry);
    titleSrc = titleUtf8.c_str();
  }
  const std::string titleEsc = xmlEscape(titleSrc, strlen(titleSrc));
  WC(zip, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
  WC(zip, "<html xmlns=\"http://www.w3.org/1999/xhtml\" xml:lang=\"zh\">\n");
  WC(zip, "<head><title>");
  zip.writeFileChunk(reinterpret_cast<const uint8_t*>(titleEsc.c_str()), titleEsc.size());
  WC(zip, "</title></head>\n<body>\n<h2>");
  zip.writeFileChunk(reinterpret_cast<const uint8_t*>(titleEsc.c_str()), titleEsc.size());
  WC(zip, "</h2>\n");

  if (txtFile.seek(begin)) {
    static uint8_t chBuf[READ_CHUNK + 1];
    std::string pendingLine;
    bool inParagraph = false;  // track whether a <p> tag is currently open
    uint8_t gbkCarry = 0;
    bool gbkHasCarry = false;
    uint32_t offset = begin;
    while (offset < end) {
      const uint32_t toRead =
          static_cast<uint32_t>(std::min(static_cast<uint32_t>(READ_CHUNK), end - offset));
      // File position advances sequentially after each read – no seek needed here.
      const int got = txtFile.read(chBuf, toRead);
      if (got <= 0) break;

      // If GBK, convert this chunk to UTF-8 before any processing.
      // After conversion, all downstream code sees clean UTF-8.
      std::string utf8Buf;
      const char* data;
      size_t dataLen;
      if (gbkTable) {
        utf8Buf = gbkChunkToUtf8(chBuf, static_cast<size_t>(got), gbkTable, gbkCarry, gbkHasCarry);
        data = utf8Buf.c_str();
        dataLen = utf8Buf.size();
        // Debug: log first conversion
        static bool logged = false;
        if (!logged && utf8Buf.size() > 0) {
          Serial.printf("[TEC] GBK->UTF8 conversion example: %.*s\n", 
                       std::min((size_t)50, utf8Buf.size()), utf8Buf.c_str());
          logged = true;
        }
      } else {
        chBuf[got] = '\0';
        data = reinterpret_cast<const char*>(chBuf);
        dataLen = static_cast<size_t>(got);
      }

      size_t pos = 0;
      while (pos < dataLen) {
        size_t lineEnd = pos;
        while (lineEnd < dataLen && data[lineEnd] != '\n') lineEnd++;
        size_t lineLen = lineEnd - pos;
        if (lineLen > 0 && data[pos + lineLen - 1] == '\r') lineLen--;  // strip CR
        pendingLine.append(data + pos, lineLen);
        if (lineEnd < dataLen) {
          // Found a newline – emit the accumulated line as a complete paragraph.
          if (!pendingLine.empty()) {
            if (!inParagraph) WC(zip, "<p>");
            const std::string esc = xmlEscape(pendingLine.c_str(), pendingLine.size());
            zip.writeFileChunk(reinterpret_cast<const uint8_t*>(esc.c_str()), esc.size());
            WC(zip, "</p>\n");
            inParagraph = false;
          } else if (inParagraph) {
            // pendingLine is empty but we had an open <p> from a previous flush
            WC(zip, "</p>\n");
            inParagraph = false;
          }
          pendingLine.clear();
          pos = lineEnd + 1;
        } else {
          // No newline in this chunk; continue accumulating.
          // Guard against unbounded growth (e.g. files with no newlines):
          // flush content every 8 KB to cap heap usage, but keep <p> open.
          if (pendingLine.size() > 8192) {
            // Back up flush point to a UTF-8 character boundary so xmlEscape
            // never sees a truncated multi-byte sequence.
            size_t flushLen = pendingLine.size();
            while (flushLen > 0 && (static_cast<unsigned char>(pendingLine[flushLen - 1]) & 0xC0) == 0x80) {
              flushLen--;  // skip continuation bytes
            }
            // If flushLen now points to a leading byte that starts an incomplete
            // sequence, back up one more so the leading byte stays in the remainder.
            if (flushLen > 0) {
              unsigned char lead = static_cast<unsigned char>(pendingLine[flushLen - 1]);
              int needed = 0;
              if (lead >= 0xF0) needed = 4;
              else if (lead >= 0xE0) needed = 3;
              else if (lead >= 0xC0) needed = 2;
              if (needed > 0 && (pendingLine.size() - (flushLen - 1)) < static_cast<size_t>(needed)) {
                flushLen--;  // keep the incomplete leading byte for next round
              }
            }
            if (flushLen > 0) {
              if (!inParagraph) WC(zip, "<p>");
              const std::string esc = xmlEscape(pendingLine.c_str(), flushLen);
              zip.writeFileChunk(reinterpret_cast<const uint8_t*>(esc.c_str()), esc.size());
              inParagraph = true;  // keep <p> open – same logical paragraph
              pendingLine.erase(0, flushLen);
            }
          }
          pos = lineEnd;
          break;
        }
      }
      offset += static_cast<uint32_t>(got);
    }
    // Flush any remaining text.
    if (!pendingLine.empty()) {
      if (!inParagraph) WC(zip, "<p>");
      const std::string esc = xmlEscape(pendingLine.c_str(), pendingLine.size());
      zip.writeFileChunk(reinterpret_cast<const uint8_t*>(esc.c_str()), esc.size());
      WC(zip, "</p>\n");
    } else if (inParagraph) {
      WC(zip, "</p>\n");
    }
    // txtFile is owned by the caller – do not close here.
  }
  WC(zip, "</body>\n</html>");
}

// Write content.opf directly into the ZIP using the streaming API.
// No large string is built in memory – each piece is flushed immediately.
void writeContentOpf(MinimalZipWriter& zip, const std::string& bookTitle, const std::string& uid,
                      int chapterCount) {
  char buf[128];
  zip.beginFileEntry("OEBPS/content.opf");
  WC(zip, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
  WC(zip,
     "<package xmlns=\"http://www.idpf.org/2007/opf\" unique-identifier=\"uid\" version=\"2.0\">\n");
  WC(zip, "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n<dc:title>");
  const std::string te = xmlEscape(bookTitle.c_str(), bookTitle.size());
  zip.writeFileChunk(reinterpret_cast<const uint8_t*>(te.c_str()), te.size());
  WC(zip, "</dc:title>\n<dc:language>zh</dc:language>\n<dc:identifier id=\"uid\">");
  zip.writeFileChunk(reinterpret_cast<const uint8_t*>(uid.c_str()), uid.size());
  WC(zip, "</dc:identifier>\n</metadata>\n<manifest>\n");
  WC(zip, "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n");
  for (int i = 0; i < chapterCount; i++) {
    snprintf(buf, sizeof(buf),
             "<item id=\"c%d\" href=\"chapter_%d.xhtml\""
             " media-type=\"application/xhtml+xml\"/>\n",
             i, i);
    zip.writeFileChunk(reinterpret_cast<const uint8_t*>(buf), strlen(buf));
  }
  WC(zip, "</manifest>\n<spine toc=\"ncx\">\n");
  for (int i = 0; i < chapterCount; i++) {
    snprintf(buf, sizeof(buf), "<itemref idref=\"c%d\"/>\n", i);
    zip.writeFileChunk(reinterpret_cast<const uint8_t*>(buf), strlen(buf));
  }
  WC(zip, "</spine>\n</package>");
  zip.endFileEntry();
}

// Write toc.ncx directly into the ZIP using the streaming API.
// Titles are loaded from Txt batch-by-batch (25 at a time) to avoid storing
// all titles in memory simultaneously.
void writeTocNcx(MinimalZipWriter& zip, const std::string& bookTitle, const std::string& uid,
                  const std::vector<ChapterMeta>& chapters, Txt& txt,
                  const uint16_t* gbkTable) {
  const int count = static_cast<int>(chapters.size());
  char buf[64];
  zip.beginFileEntry("OEBPS/toc.ncx");
  WC(zip, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
  WC(zip,
     "<!DOCTYPE ncx PUBLIC \"-//NISO//DTD ncx 2005-1//EN\" "
     "\"http://www.daisy.org/z3986/2005/ncx-2005-1.dtd\">\n");
  WC(zip,
     "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\">\n<head>\n");
  WC(zip, "<meta name=\"dtb:uid\" content=\"");
  zip.writeFileChunk(reinterpret_cast<const uint8_t*>(uid.c_str()), uid.size());
  WC(zip, "\"/>\n<meta name=\"dtb:depth\" content=\"1\"/>\n");
  WC(zip, "<meta name=\"dtb:totalPageCount\" content=\"0\"/>\n");
  WC(zip, "<meta name=\"dtb:maxPageNumber\" content=\"0\"/>\n</head>\n");
  WC(zip, "<docTitle><text>");
  const std::string te = xmlEscape(bookTitle.c_str(), bookTitle.size());
  zip.writeFileChunk(reinterpret_cast<const uint8_t*>(te.c_str()), te.size());
  WC(zip, "</text></docTitle>\n<navMap>\n");
  int lastBatchStart = -1;
  for (int i = 0; i < count; i++) {
    // Ensure the correct batch is loaded for this chapter's title
    int batchStart = (chapters[i].index / 25) * 25;
    if (batchStart != lastBatchStart) {
      txt.parseChapterIndexAndOffset(batchStart);
      lastBatchStart = batchStart;
    }
    snprintf(buf, sizeof(buf), "<navPoint id=\"np%d\" playOrder=\"%d\">", i, i + 1);
    zip.writeFileChunk(reinterpret_cast<const uint8_t*>(buf), strlen(buf));
    WC(zip, "<navLabel><text>");
    const char* titlePtr = txt.getChapterTitlePtr(chapters[i].index);
    char fallback[32];
    if (!titlePtr || titlePtr[0] == '\0') {
      snprintf(fallback, sizeof(fallback), "\xe7\xac\xac%d\xe7\xab\xa0", chapters[i].index + 1);
      titlePtr = fallback;
    }
    // 注意：章节标题已经是UTF-8编码（Txt类在解析时已从UTF-8缓存读取）
    // 不需要再次进行GBK→UTF-8转换，否则会导致UTF-8被当作GBK转换产生乱码
    const std::string chEsc = xmlEscape(titlePtr, strlen(titlePtr));
    zip.writeFileChunk(reinterpret_cast<const uint8_t*>(chEsc.c_str()), chEsc.size());
    WC(zip, "</text></navLabel><content src=\"chapter_");
    snprintf(buf, sizeof(buf), "%d.xhtml\"/></navPoint>\n", i);
    zip.writeFileChunk(reinterpret_cast<const uint8_t*>(buf), strlen(buf));
  }
  WC(zip, "</navMap>\n</ncx>");
  zip.endFileEntry();
}

#undef WC

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::string TxtToEpubConverter::getCachedEpubPath(const std::string& txtPath,
                                                    const std::string& cacheDir) {
  return txtCachePath(txtPath, cacheDir) + "/book.epub";
}

bool TxtToEpubConverter::isCacheValid(const std::string& txtPath, const std::string& cacheDir) {
  const std::string epubPath = getCachedEpubPath(txtPath, cacheDir);
  const std::string metaPath = txtCachePath(txtPath, cacheDir) + "/epub.meta";

  if (!SdMan.exists(epubPath.c_str()) || !SdMan.exists(metaPath.c_str())) {
    Serial.printf("[TEC] Cache invalid: epub=%d, meta=%d\n", 
                  SdMan.exists(epubPath.c_str()), SdMan.exists(metaPath.c_str()));
    return false;
  }

  // Read stored TXT size and conversion version from meta file
  FsFile meta;
  if (!SdMan.openFileForRead("TEC", metaPath, meta)) {
    Serial.printf("[TEC] Cache invalid: cannot open meta file\n");
    return false;
  }
  uint32_t storedSize = 0;
  uint32_t storedVersion = 0;
  serialization::readPod(meta, storedSize);
  serialization::readPod(meta, storedVersion);
  meta.close();

  // Compare with current TXT file size and code version
  FsFile txt;
  if (!SdMan.openFileForRead("TEC", txtPath, txt)) {
    Serial.printf("[TEC] Cache invalid: cannot open txt file\n");
    return false;
  }
  const uint32_t currentSize = static_cast<uint32_t>(txt.size());
  txt.close();

  bool valid = (storedSize == currentSize && storedSize > 0 && storedVersion == EPUB_CONVERSION_VERSION);
  Serial.printf("[TEC] Cache check: storedSize=%u, currentSize=%u, storedVer=%u, currentVer=%u, valid=%d\n",
                storedSize, currentSize, storedVersion, EPUB_CONVERSION_VERSION, valid);
  return valid;
}

bool TxtToEpubConverter::convert(const std::string& txtPath, const std::string& cacheDir,
                                   const std::function<void(int)>& progressCb) {
  Serial.printf("[%lu] [TEC] Starting TXT\u2192EPUB conversion: %s\n", millis(), txtPath.c_str());
  if (progressCb) progressCb(0);

  // -----------------------------------------------------------------------
  // Phase 1: scan all chapters.
  // Txt object is kept alive through Phase 2 for on-demand title retrieval
  // from its chapter cache (loaded 25 at a time).  The Txt object itself is
  // small (~3 KB); the big saving is that we no longer store per-chapter
  // title strings in the chapters vector.
  // -----------------------------------------------------------------------
  std::string bookTitle;
  uint32_t    fileSize = 0;
  const std::string cachePath = txtCachePath(txtPath, cacheDir);
  const std::string epubPath  = cachePath + "/book.epub";
  const std::string uid       = "txt-" + std::to_string(std::hash<std::string>{}(txtPath));
  std::vector<ChapterMeta> chapters;
  chapters.reserve(128);  // Pre-allocate for typical books; vector will grow if needed

  Txt txt(txtPath, cacheDir);
  if (!txt.load()) {
    Serial.printf("[%lu] [TEC] Failed to load TXT file\n", millis());
    return false;
  }
  txt.setupCacheDir();

  // -----------------------------------------------------------------------
  // Encoding detection: sample first 4 KB to decide UTF-8 vs GBK.
  // If GBK, use the flash-resident lookup table (zero heap cost).
  // -----------------------------------------------------------------------
  const uint16_t* gbkTable = nullptr;
  {
    FsFile probe;
    if (SdMan.openFileForRead("TEC", txtPath, probe)) {
      static uint8_t sample[512];  // static to avoid stack pressure (loopTask = 8 KB)
      const int n = probe.read(sample, sizeof(sample));
      probe.close();
      if (n > 0 && !isValidUtf8(sample, static_cast<size_t>(n))) {
        Serial.printf("[%lu] [TEC] Detected GBK encoding\n", millis());
        gbkTable = getGbkTable();
      } else {
        Serial.printf("[%lu] [TEC] Encoding: UTF-8\n", millis());
      }
    }
  }

  bookTitle = txt.getTitle();
  // 注意：getTitle() 现在已经返回UTF-8编码的书名（在Txt类内部已处理GBK转换）
  // 不需要再次进行GBK→UTF-8转换
  
  fileSize  = static_cast<uint32_t>(txt.getFileSize());

  for (int batchStart = 0; ; batchStart += 25) {
    txt.parseChapterIndexAndOffset(batchStart);
    // Phase 1 progress: 0% → 45%
    if (progressCb) progressCb(std::min(1 + batchStart / 6, 45));
    bool foundAny = false;
    for (int i = 0; i < 25; i++) {
      const int chIdx = batchStart + i;
      if (!txt.isChapterExist(chIdx)) break;
      foundAny = true;
      uint32_t begin = txt.getChapterOffsetByIndex(chIdx);
      uint32_t end   = txt.getChapterendOffsetByIndex(chIdx);
      if (end == 0 || end <= begin) end = fileSize;
      chapters.push_back({chIdx, begin, end});
    }
    if (!foundAny) break;
    if (!txt.isChapterExist(batchStart + 24)) break;
  }

  if (chapters.empty()) {
    Serial.printf("[%lu] [TEC] No chapters found, treating whole file as one chapter\n", millis());
    chapters.push_back({0, 0, fileSize});
  }

  // Remove stale EPUB if present
  if (SdMan.exists(epubPath.c_str())) {
    SdMan.remove(epubPath.c_str());
  }

  // Also clear the Epub-level cache (section layouts, metadata) so stale
  // pages aren't served after the EPUB is regenerated.
  {
    const std::string epubCachePath =
        std::string(cacheDir) + "/epub_" + std::to_string(std::hash<std::string>{}(epubPath));
    if (SdMan.exists(epubCachePath.c_str())) {
      SdMan.removeDir(epubCachePath.c_str());
      Serial.printf("[%lu] [TEC] Cleared Epub cache: %s\n", millis(), epubCachePath.c_str());
    }
  }

  const int totalChapters = static_cast<int>(chapters.size());
  Serial.printf("[%lu] [TEC] Found %d chapters, generating EPUB...\n", millis(), totalChapters);
  if (progressCb) progressCb(50);  // Phase 1 结束，Phase 2 开始

  // -----------------------------------------------------------------------
  // Phase 2: write EPUB ZIP
  // -----------------------------------------------------------------------
  MinimalZipWriter zip;
  if (!zip.begin(epubPath)) {
    Serial.printf("[%lu] [TEC] Failed to create EPUB output file\n", millis());
    return false;
  }

  // 1. mimetype – MUST be first, STORE, no extra fields (EPUB spec §3.3)
  {
    const char* mt = "application/epub+zip";
    zip.addFile("mimetype", reinterpret_cast<const uint8_t*>(mt), static_cast<uint32_t>(strlen(mt)));
  }

  // 2. META-INF/container.xml
  {
    const char* container =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<container version=\"1.0\""
        " xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
        "  <rootfiles>\n"
        "    <rootfile full-path=\"OEBPS/content.opf\""
        " media-type=\"application/oebps-package+xml\"/>\n"
        "  </rootfiles>\n"
        "</container>";
    zip.addFile("META-INF/container.xml", reinterpret_cast<const uint8_t*>(container),
                static_cast<uint32_t>(strlen(container)));
  }

  // 3. content.opf – streamed directly into ZIP (no large in-memory string)
  writeContentOpf(zip, bookTitle, uid, totalChapters);

  // 4. toc.ncx – streamed directly into ZIP; titles loaded from Txt cache on demand
  writeTocNcx(zip, bookTitle, uid, chapters, txt, gbkTable);

  // 5. Chapter XHTML files – TXT file opened ONCE for all chapters.
  //    Avoids N file-open/close pairs (e.g. 405 opens × ~10ms ≈ 4s saved).
  FsFile txtFile;
  if (!SdMan.openFileForRead("TEC", txtPath, txtFile)) {
    Serial.printf("[%lu] [TEC] Failed to open TXT file for XHTML writing\n", millis());
    zip.end();
    return false;
  }
  int lastTitleBatch = -1;
  for (int i = 0; i < totalChapters; i++) {
    const auto& ch = chapters[i];
    char name[48];
    snprintf(name, sizeof(name), "OEBPS/chapter_%d.xhtml", i);

    // Load the correct batch for this chapter's title
    int batchStart = (ch.index / 25) * 25;
    if (batchStart != lastTitleBatch) {
      txt.parseChapterIndexAndOffset(batchStart);
      lastTitleBatch = batchStart;
    }
    const char* titlePtr = txt.getChapterTitlePtr(ch.index);
    char fallbackTitle[32];
    if (!titlePtr || titlePtr[0] == '\0') {
      snprintf(fallbackTitle, sizeof(fallbackTitle), "\xe7\xac\xac%d\xe7\xab\xa0", ch.index + 1);
      titlePtr = fallbackTitle;
    }

    // 注意：章节标题已经是UTF-8编码（Txt类在解析时已从UTF-8缓存读取）
    // 无论是否是分卷模式，标题都是UTF-8，不需要再次转换
    const bool titleIsUtf8 = true;
    zip.beginFileEntry(name);
    writeChapterXhtml(zip, titlePtr, txtFile, ch.begin, ch.end, gbkTable, titleIsUtf8);
    zip.endFileEntry();

    if (progressCb) {
      progressCb(50 + (i + 1) * 50 / totalChapters);  // Phase 2: 50% → 100%
    }

    // Yield to other tasks periodically
    if (i % 10 == 0) {
      vTaskDelay(1 / portTICK_PERIOD_MS);
    }
  }
  txtFile.close();

  if (!zip.end()) {
    Serial.printf("[%lu] [TEC] Failed to finalise EPUB ZIP\n", millis());
    return false;
  }

  // ---------- Phase 4: write epub.meta (stores TXT file size for cache validation) ----------
  {
    const std::string metaPath = cachePath + "/epub.meta";
    FsFile meta;
    if (SdMan.openFileForWrite("TEC", metaPath, meta)) {
      serialization::writePod(meta, fileSize);
      serialization::writePod(meta, EPUB_CONVERSION_VERSION);
      meta.sync();
      meta.close();
    }
  }

  Serial.printf("[%lu] [TEC] Conversion complete: %s (%d chapters)\n", millis(), epubPath.c_str(),
                totalChapters);
  return true;
}
