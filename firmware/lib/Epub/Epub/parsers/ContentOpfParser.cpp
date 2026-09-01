#include "ContentOpfParser.h"

#include <HardwareSerial.h>

#include <cstring>
#include <cstdlib>

#include "../BookMetadataCache.h"

namespace {
constexpr char MEDIA_TYPE_NCX[] = "application/x-dtbncx+xml";
constexpr char MEDIA_TYPE_CSS[] = "text/css";
constexpr char itemCacheFile[] = "/.items.bin";
}  // namespace

// ---------------------------------------------------------------------------
// Static helper implementations
// ---------------------------------------------------------------------------

void ContentOpfParser::normalisePathInto(char* out, size_t outSize,
                                          const char* base, size_t baseLen,
                                          const char* rel) {
  // Build combined path on a stack buffer, then resolve '..' in-place.
  char work[512];
  size_t relLen = strlen(rel);
  if (baseLen + relLen + 1 >= sizeof(work)) {
    snprintf(out, outSize, "%s%s", base, rel);
    return;
  }
  memcpy(work, base, baseLen);
  memcpy(work + baseLen, rel, relLen + 1);  // includes null terminator

  // Tokenise by '/' and collect non-'..' segments
  const char* segs[64];
  int nsegs = 0;
  char* p = work;
  while (*p) {
    while (*p == '/') *p++ = '\0';  // null-terminate previous segment and advance
    if (!*p) break;
    char* segStart = p;
    while (*p && *p != '/') p++;
    if (strcmp(segStart, "..") == 0) {
      if (nsegs > 0) nsegs--;
    } else if (strcmp(segStart, ".") != 0 && *segStart) {
      if (nsegs < 64) segs[nsegs++] = segStart;
    }
  }

  size_t pos = 0;
  for (int i = 0; i < nsegs; i++) {
    if (i > 0 && pos + 1 < outSize) out[pos++] = '/';
    const char* s = segs[i];
    while (*s && pos + 1 < outSize) out[pos++] = *s++;
  }
  out[pos] = '\0';
}

void ContentOpfParser::writeStringChar(FsFile& file, const char* s) {
  const uint32_t len = static_cast<uint32_t>(strlen(s));
  file.write(reinterpret_cast<const uint8_t*>(&len), sizeof(len));
  if (len > 0) file.write(reinterpret_cast<const uint8_t*>(s), len);
}

bool ContentOpfParser::readStringToChar(FsFile& file, char* buf, size_t bufSize) {
  uint32_t len;
  if (file.read(reinterpret_cast<uint8_t*>(&len), sizeof(len)) != (int)sizeof(len)) {
    if (bufSize > 0) buf[0] = '\0';
    return false;
  }
  if (len >= bufSize) {
    uint32_t toRead = static_cast<uint32_t>(bufSize - 1);
    file.read(reinterpret_cast<uint8_t*>(buf), toRead);
    buf[toRead] = '\0';
    file.seekCur(len - toRead);
    return false;
  }
  if (len > 0 && file.read(reinterpret_cast<uint8_t*>(buf), len) != (int)len) {
    buf[0] = '\0';
    return false;
  }
  buf[len] = '\0';
  return true;
}

bool ContentOpfParser::skipString(FsFile& file) {
  uint32_t len;
  if (file.read(reinterpret_cast<uint8_t*>(&len), sizeof(len)) != (int)sizeof(len)) return false;
  if (len > 0) file.seekCur(len);
  return true;
}

bool ContentOpfParser::setup() {
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    Serial.printf("[%lu] [COF] Couldn't allocate memory for parser\n", millis());
    return false;
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
  return true;
}

ContentOpfParser::~ContentOpfParser() {
  if (parser) {
    XML_StopParser(parser, XML_FALSE);
    XML_SetElementHandler(parser, nullptr, nullptr);
    XML_SetCharacterDataHandler(parser, nullptr);
    XML_ParserFree(parser);
    parser = nullptr;
  }
  if (tempItemStore) {
    tempItemStore.close();
  }
  if (SdMan.exists((cachePath + itemCacheFile).c_str())) {
    SdMan.remove((cachePath + itemCacheFile).c_str());
  }
  if (indexStore) {
    indexStore.close();
  }
  if (SdMan.exists((cachePath + indexCacheFile).c_str())) {
    SdMan.remove((cachePath + indexCacheFile).c_str());
  }
  if (ramIndex) {
    free(ramIndex);
    ramIndex = nullptr;
  }
}

size_t ContentOpfParser::write(const uint8_t data) { return write(&data, 1); }

size_t ContentOpfParser::write(const uint8_t* buffer, const size_t size) {
  if (!parser) return 0;

  const uint8_t* currentBufferPos = buffer;
  auto remainingInBuffer = size;

  while (remainingInBuffer > 0) {
    void* const buf = XML_GetBuffer(parser, 1024);

    if (!buf) {
      Serial.printf("[%lu] [COF] Couldn't allocate memory for buffer\n", millis());
      XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
      XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      parser = nullptr;
      return 0;
    }

    const auto toRead = remainingInBuffer < 1024 ? remainingInBuffer : 1024;
    memcpy(buf, currentBufferPos, toRead);

    if (XML_ParseBuffer(parser, static_cast<int>(toRead), remainingSize == toRead) == XML_STATUS_ERROR) {
      Serial.printf("[%lu] [COF] Parse error at line %lu: %s\n", millis(), XML_GetCurrentLineNumber(parser),
                    XML_ErrorString(XML_GetErrorCode(parser)));
      XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
      XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      parser = nullptr;
      return 0;
    }

    currentBufferPos += toRead;
    remainingInBuffer -= toRead;
    remainingSize -= toRead;
  }

  return size;
}

void XMLCALL ContentOpfParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ContentOpfParser*>(userData);
  // All branches use only stack-allocated buffers (char[]) or fixed-size SD file writes.
  // No std::string local variables, no std::vector insertions → zero heap allocation in callbacks.

  if (self->state == START && (strcmp(name, "package") == 0 || strcmp(name, "opf:package") == 0)) {
    self->state = IN_PACKAGE;
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "metadata") == 0 || strcmp(name, "opf:metadata") == 0)) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:title") == 0) {
    self->state = IN_BOOK_TITLE;
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:creator") == 0) {
    self->state = IN_BOOK_AUTHOR;
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:language") == 0) {
    self->state = IN_BOOK_LANGUAGE;
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "manifest") == 0 || strcmp(name, "opf:manifest") == 0)) {
    self->state = IN_MANIFEST;
    char itemPath[256];
    snprintf(itemPath, sizeof(itemPath), "%s%s", self->cachePath.c_str(), itemCacheFile);
    if (!SdMan.openFileForWrite("COF", itemPath, self->tempItemStore)) {
      Serial.printf("[%lu] [COF] Couldn't open temp items file for writing.\n", millis());
    }
    char indexPath[256];
    snprintf(indexPath, sizeof(indexPath), "%s%s", self->cachePath.c_str(), indexCacheFile);
    if (!SdMan.openFileForWrite("COF", indexPath, self->indexStore)) {
      Serial.printf("[%lu] [COF] Couldn't open index file for writing.\n", millis());
    }
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "spine") == 0 || strcmp(name, "opf:spine") == 0)) {
    self->state = IN_SPINE;
    // Sort index entries on SD card for O(log n) itemref lookup
    if (self->indexEntryCount >= LARGE_SPINE_THRESHOLD) {
      const size_t totalBytes = self->indexEntryCount * sizeof(ItemIndexEntry);
      auto* entries = static_cast<ItemIndexEntry*>(malloc(totalBytes));
      if (entries) {
        char indexPath[256];
        snprintf(indexPath, sizeof(indexPath), "%s%s", self->cachePath.c_str(), indexCacheFile);
        if (SdMan.openFileForRead("COF", indexPath, self->indexStore)) {
          self->indexStore.read(reinterpret_cast<uint8_t*>(entries), totalBytes);
          self->indexStore.close();
        }
        qsort(entries, self->indexEntryCount, sizeof(ItemIndexEntry), [](const void* a, const void* b) -> int {
          const auto* ea = static_cast<const ItemIndexEntry*>(a);
          const auto* eb = static_cast<const ItemIndexEntry*>(b);
          if (ea->idHash < eb->idHash) return -1;
          if (ea->idHash > eb->idHash) return 1;
          if (ea->idLen < eb->idLen) return -1;
          if (ea->idLen > eb->idLen) return 1;
          return 0;
        });
        if (SdMan.openFileForWrite("COF", indexPath, self->indexStore)) {
          self->indexStore.write(reinterpret_cast<const uint8_t*>(entries), totalBytes);
          self->indexStore.close();
        }
        // Keep sorted array in RAM — fastest O(log n) lookup, zero SD seeks
        self->ramIndex = entries;  // freed in endElement(spine) or destructor
        Serial.printf("[%lu] [COF] RAM index ready: %lu entries (%u bytes)\n", millis(),
                      static_cast<unsigned long>(self->indexEntryCount),
                      static_cast<unsigned>(totalBytes));
      } else {
        Serial.printf("[%lu] [COF] malloc(%u) failed, falling back to linear scan\n", millis(),
                      static_cast<unsigned>(totalBytes));
      }
    }
    // Reopen temp item store for random-access reads during spine pass
    char itemPath[256];
    snprintf(itemPath, sizeof(itemPath), "%s%s", self->cachePath.c_str(), itemCacheFile);
    if (!SdMan.openFileForRead("COF", itemPath, self->tempItemStore)) {
      Serial.printf("[%lu] [COF] Couldn't open temp items file for reading.\n", millis());
    }
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "guide") == 0 || strcmp(name, "opf:guide") == 0)) {
    self->state = IN_GUIDE;
    return;
  }

  if (self->state == IN_METADATA && (strcmp(name, "meta") == 0 || strcmp(name, "opf:meta") == 0)) {
    bool isCover = false;
    char coverIdBuf[128] = {};
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "name") == 0 && strcmp(atts[i + 1], "cover") == 0) {
        isCover = true;
      } else if (strcmp(atts[i], "content") == 0) {
        strncpy(coverIdBuf, atts[i + 1], sizeof(coverIdBuf) - 1);
      }
    }
    if (isCover) {
      strncpy(self->coverItemId, coverIdBuf, sizeof(self->coverItemId) - 1);
    }
    return;
  }

  if (self->state == IN_MANIFEST && (strcmp(name, "item") == 0 || strcmp(name, "opf:item") == 0)) {
    // All stack buffers — zero heap allocation
    char itemId[256] = {};
    char href[512] = {};
    char mediaType[64] = {};
    char properties[64] = {};

    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "id") == 0) {
        strncpy(itemId, atts[i + 1], sizeof(itemId) - 1);
      } else if (strcmp(atts[i], "href") == 0) {
        normalisePathInto(href, sizeof(href), self->baseContentPath.c_str(), self->baseContentPath.size(),
                          atts[i + 1]);
      } else if (strcmp(atts[i], "media-type") == 0) {
        strncpy(mediaType, atts[i + 1], sizeof(mediaType) - 1);
      } else if (strcmp(atts[i], "properties") == 0) {
        strncpy(properties, atts[i + 1], sizeof(properties) - 1);
      }
    }

    // Write fixed-size index entry to SD (no heap allocation)
    if (self->indexStore) {
      ItemIndexEntry entry;
      entry.idHash = fnvHashCStr(itemId);
      entry.idLen = static_cast<uint16_t>(strlen(itemId));
      entry.fileOffset = static_cast<uint32_t>(self->tempItemStore.position());
      self->indexStore.write(reinterpret_cast<const uint8_t*>(&entry), sizeof(entry));
      self->indexEntryCount++;
    }

    // Write id + href to temp item store (length-prefixed, no heap)
    writeStringChar(self->tempItemStore, itemId);
    writeStringChar(self->tempItemStore, href);

    if (strcmp(itemId, self->coverItemId) == 0) {
      self->coverItemHref = href;  // single assign to public member
    }

    if (strcmp(mediaType, MEDIA_TYPE_NCX) == 0) {
      if (self->tocNcxPath.empty()) {
        self->tocNcxPath = href;
      } else {
        Serial.printf("[%lu] [COF] Warning: Multiple NCX files found. Ignoring: %s\n", millis(), href);
      }
    }

    if (strcmp(mediaType, MEDIA_TYPE_CSS) == 0) {
      self->cssFiles.push_back(href);  // few CSS files per EPUB, low OOM risk
    }

    if (properties[0] != '\0' && self->tocNavPath.empty()) {
      if (strcmp(properties, "nav") == 0 || strncmp(properties, "nav ", 4) == 0 ||
          strstr(properties, " nav") != nullptr) {
        self->tocNavPath = href;
        Serial.printf("[%lu] [COF] Found EPUB3 nav: %s\n", millis(), href);
      }
    }
    return;
  }

  // NOTE: spine must appear after manifest (EPUB spec guarantees this)
  if (self->cache) {
    if (self->state == IN_SPINE && (strcmp(name, "itemref") == 0 || strcmp(name, "opf:itemref") == 0)) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "idref") == 0) {
          const char* idref = atts[i + 1];
          char href[512] = {};
          bool found = false;

          if (self->ramIndex) {
            // In-RAM binary search — O(log n), fastest (no SD seeks)
            const uint32_t targetHash = fnvHashCStr(idref);
            const uint16_t targetLen = static_cast<uint16_t>(strlen(idref));
            int32_t lo = 0;
            int32_t hi = static_cast<int32_t>(self->indexEntryCount) - 1;
            while (lo <= hi) {
              const int32_t mid = lo + (hi - lo) / 2;
              const ItemIndexEntry& entry = self->ramIndex[mid];
              if (entry.idHash < targetHash || (entry.idHash == targetHash && entry.idLen < targetLen)) {
                lo = mid + 1;
              } else if (entry.idHash > targetHash || (entry.idHash == targetHash && entry.idLen > targetLen)) {
                hi = mid - 1;
              } else {
                // Hash+len match: verify exact string to rule out collisions
                self->tempItemStore.seek(entry.fileOffset);
                char storedId[256] = {};
                readStringToChar(self->tempItemStore, storedId, sizeof(storedId));
                if (strcmp(storedId, idref) == 0) {
                  readStringToChar(self->tempItemStore, href, sizeof(href));
                  found = true;
                }
                break;
              }
            }
          } else {
            // Linear scan fallback (small manifests or malloc failed)
            self->tempItemStore.seek(0);
            char itemId[256] = {};
            while (self->tempItemStore.available()) {
              if (!readStringToChar(self->tempItemStore, itemId, sizeof(itemId))) break;
              if (!readStringToChar(self->tempItemStore, href, sizeof(href))) break;
              if (strcmp(itemId, idref) == 0) {
                found = true;
                break;
              }
            }
          }

          if (found && self->cache) {
            self->cache->createSpineEntry(href);
          }
        }
      }
      return;
    }
  }

  if (self->state == IN_GUIDE && (strcmp(name, "reference") == 0 || strcmp(name, "opf:reference") == 0)) {
    char type[64] = {};
    char textHref[512] = {};
    bool typeOk = false;
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "type") == 0) {
        strncpy(type, atts[i + 1], sizeof(type) - 1);
        typeOk = (strcmp(type, "text") == 0 || strcmp(type, "start") == 0);
        if (!typeOk) {
          Serial.printf("[%lu] [COF] Skipping non-text reference in guide: %s\n", millis(), type);
          break;
        }
      } else if (strcmp(atts[i], "href") == 0) {
        normalisePathInto(textHref, sizeof(textHref), self->baseContentPath.c_str(),
                          self->baseContentPath.size(), atts[i + 1]);
      }
    }
    if (typeOk && textHref[0] != '\0') {
      if (strcmp(type, "text") == 0 || (strcmp(type, "start") == 0 && !self->textReferenceHref.empty())) {
        Serial.printf("[%lu] [COF] Found %s reference in guide: %s.\n", millis(), type, textHref);
        self->textReferenceHref = textHref;
      }
    }
    return;
  }
}

void XMLCALL ContentOpfParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ContentOpfParser*>(userData);
  if (self->state == IN_BOOK_TITLE) {
    self->title.append(s, len);
    return;
  }
  if (self->state == IN_BOOK_AUTHOR) {
    self->author.append(s, len);
    return;
  }
  if (self->state == IN_BOOK_LANGUAGE) {
    self->language.append(s, len);
    return;
  }
}

void XMLCALL ContentOpfParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ContentOpfParser*>(userData);
  (void)name;

  if (self->state == IN_SPINE && (strcmp(name, "spine") == 0 || strcmp(name, "opf:spine") == 0)) {
    self->state = IN_PACKAGE;
    self->tempItemStore.close();
    self->indexStore.close();
    // Release RAM index after spine pass is complete
    if (self->ramIndex) {
      free(self->ramIndex);
      self->ramIndex = nullptr;
    }
    return;
  }

  if (self->state == IN_GUIDE && (strcmp(name, "guide") == 0 || strcmp(name, "opf:guide") == 0)) {
    self->state = IN_PACKAGE;
    return;
  }

  if (self->state == IN_MANIFEST && (strcmp(name, "manifest") == 0 || strcmp(name, "opf:manifest") == 0)) {
    self->state = IN_PACKAGE;
    self->tempItemStore.close();
    self->indexStore.close();
    return;
  }

  if (self->state == IN_BOOK_TITLE && strcmp(name, "dc:title") == 0) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_AUTHOR && strcmp(name, "dc:creator") == 0) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_LANGUAGE && strcmp(name, "dc:language") == 0) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_METADATA && (strcmp(name, "metadata") == 0 || strcmp(name, "opf:metadata") == 0)) {
    self->state = IN_PACKAGE;
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "package") == 0 || strcmp(name, "opf:package") == 0)) {
    self->state = START;
    return;
  }
}
