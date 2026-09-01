#include "Section.h"

#include <SDCardManager.h>
#include <Serialization.h>
#include <ctype.h>

#include "Page.h"
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"

// ---------------------------------------------------------------------------
// HTML → XHTML preprocessor
// 1. Void-element fixer: <br> → <br/>  <img src="x"> → <img src="x"/>
// 2. HTML entity fixer:  &nbsp; → &#160;   bare & → &amp;
// Both passes run in a single scan to avoid a second temp file on SD card.
// ---------------------------------------------------------------------------
namespace {

static bool isVoidElement(const char* name, int len) {
  static const char* VOID[] = {
      "area", "base", "br",    "col",   "embed",
      "hr",   "img",  "input", "link",  "meta",
      "param","source","track","wbr"
  };
  for (const char* v : VOID) {
    int vl = static_cast<int>(strlen(v));
    if (vl == len && strncasecmp(name, v, len) == 0) return true;
  }
  return false;
}

// HTML named character entity → Unicode code point.
// Only non-XML entities are listed here; the five XML built-ins
// (amp lt gt quot apos) and numeric refs (&#N; / &#xN;) are passed through
// unchanged by the caller.
struct HtmlEntityEntry { const char* name; uint32_t codepoint; };
static const HtmlEntityEntry HTML_ENTITIES[] = {
    // Non-breaking / soft spaces
    {"nbsp",    160}, {"shy",     173}, {"thinsp", 8201},
    {"ensp",   8194}, {"emsp",   8195},
    // Zero-width / directional marks
    {"zwnj",   8204}, {"zwj",    8205}, {"lrm",    8206}, {"rlm",    8207},
    // Dashes & ellipsis
    {"ndash",  8211}, {"mdash",  8212}, {"hellip", 8230},
    // Quotation marks
    {"ldquo",  8220}, {"rdquo",  8221}, {"lsquo",  8216}, {"rsquo",  8217},
    {"sbquo",  8218}, {"bdquo",  8222},
    {"lsaquo", 8249}, {"rsaquo", 8250},
    {"laquo",   171}, {"raquo",   187},
    // Common symbols
    {"copy",    169}, {"reg",     174}, {"trade",  8482},
    {"euro",   8364}, {"pound",   163}, {"yen",     165}, {"cent",    162},
    {"deg",     176}, {"para",    182}, {"sect",    167},
    {"dagger", 8224}, {"Dagger", 8225},
    {"middot",  183}, {"bull",   8226}, {"prime",  8242}, {"Prime",  8243},
    {"frasl",  8260}, {"oline",  8254},
    // Math
    {"plusmn",  177}, {"times",   215}, {"divide",  247},
    {"frac12",  189}, {"frac14",  188}, {"frac34",  190},
    {"sup2",    178}, {"sup3",    179},
    // Latin-1 supplement (accented chars used in European languages)
    {"iexcl",  161}, {"iquest", 191}, {"uml",    168}, {"ordf",   170},
    {"ordm",   186}, {"macr",   175}, {"acute",  180}, {"cedil",  184},
    {"micro",  181}, {"brvbar", 166}, {"curren", 164},
    {"Agrave", 192}, {"Aacute", 193}, {"Acirc",  194}, {"Atilde", 195},
    {"Auml",   196}, {"Aring",  197}, {"AElig",  198}, {"Ccedil", 199},
    {"Egrave", 200}, {"Eacute", 201}, {"Ecirc",  202}, {"Euml",   203},
    {"Igrave", 204}, {"Iacute", 205}, {"Icirc",  206}, {"Iuml",   207},
    {"ETH",    208}, {"Ntilde", 209}, {"Ograve", 210}, {"Oacute", 211},
    {"Ocirc",  212}, {"Otilde", 213}, {"Ouml",   214}, {"Oslash", 216},
    {"Ugrave", 217}, {"Uacute", 218}, {"Ucirc",  219}, {"Uuml",   220},
    {"Yacute", 221}, {"THORN",  222}, {"szlig",  223},
    {"agrave", 224}, {"aacute", 225}, {"acirc",  226}, {"atilde", 227},
    {"auml",   228}, {"aring",  229}, {"aelig",  230}, {"ccedil", 231},
    {"egrave", 232}, {"eacute", 233}, {"ecirc",  234}, {"euml",   235},
    {"igrave", 236}, {"iacute", 237}, {"icirc",  238}, {"iuml",   239},
    {"eth",    240}, {"ntilde", 241}, {"ograve", 242}, {"oacute", 243},
    {"ocirc",  244}, {"otilde", 245}, {"ouml",   246}, {"oslash", 248},
    {"ugrave", 249}, {"uacute", 250}, {"ucirc",  251}, {"uuml",   252},
    {"yacute", 253}, {"thorn",  254}, {"yuml",   255},
    // Greek letters (science/math books)
    {"alpha",  945}, {"beta",   946}, {"gamma",  947}, {"delta",  948},
    {"epsilon",949}, {"theta",  952}, {"lambda", 955}, {"mu",     956},
    {"pi",     960}, {"sigma",  963}, {"tau",    964}, {"phi",    966},
    {"omega",  969},
    // Card suits
    {"spades", 9824}, {"clubs",  9827}, {"hearts", 9829}, {"diams",  9830},
};

static uint32_t lookupHtmlEntity(const char* name, int len) {
  constexpr int N = static_cast<int>(sizeof(HTML_ENTITIES) / sizeof(HTML_ENTITIES[0]));
  for (int i = 0; i < N; i++) {
    const char* n = HTML_ENTITIES[i].name;
    int nl = static_cast<int>(strlen(n));
    if (nl == len && strncmp(n, name, len) == 0) return HTML_ENTITIES[i].codepoint;
  }
  return 0;
}

// Single-pass HTML → XHTML preprocessor.
// Fixes void elements AND replaces non-XML HTML entities so that expat
// (strict XML parser) can parse the chapter without errors.
// Uses 512-byte buffered I/O to minimise SD card overhead.
static bool preprocessHtmlVoidElements(const std::string& src, const std::string& dst) {
  FsFile inFile, outFile;
  if (!SdMan.openFileForRead("SCT", src, inFile)) return false;
  if (!SdMan.openFileForWrite("SCT", dst, outFile)) {
    inFile.close();
    return false;
  }

  constexpr int BUF_SIZE = 512;
  uint8_t inBuf[BUF_SIZE];
  uint8_t outBuf[BUF_SIZE];
  int outLen = 0;

  enum State { TEXT, IN_TAG, IN_ENTITY } state = TEXT;
  // --- void-element tracking ---
  char tagName[32] = {};
  int  tagNameLen   = 0;
  bool gatheringName = false;
  bool isClosingTag  = false;
  bool isVoid        = false;
  bool passthrough   = false;  // inside <! or <?
  char prevCh        = 0;
  // --- entity tracking ---
  char entityBuf[16] = {};
  int  entityBufLen  = 0;
  State entityPrevState = TEXT;
  // --- unquoted attribute value tracking ---
  // Tracks where we are inside a tag's attribute list so that unquoted
  // attribute values (e.g. charset=UTF-8) can be wrapped in double-quotes.
  enum AttrState {
    AS_NONE,       // tag name phase or passthrough / closing tag
    AS_START,      // between attributes
    AS_IN_NAME,    // inside attribute name
    AS_AFTER_EQ,   // just saw '=', expecting a value
    AS_IN_DQUOTE,  // inside a double-quoted value
    AS_IN_SQUOTE,  // inside a single-quoted value
    AS_IN_UNQUOTED // inside an unquoted value (we inserted opening '"')
  } attrState = AS_NONE;
  bool ok = true;

  // Write one byte to the output buffer, flushing to SD when full.
  auto writeOut = [&](uint8_t c) -> bool {
    outBuf[outLen++] = c;
    if (outLen >= BUF_SIZE) {
      if (outFile.write(outBuf, BUF_SIZE) != BUF_SIZE) return false;
      outLen = 0;
    }
    return true;
  };
  // Write a NUL-terminated C-string to output.
  auto writeStr = [&](const char* s) {
    while (*s && ok) ok = writeOut(static_cast<uint8_t>(*s++));
  };
  auto isAttrSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };

  while (ok) {
    const int readLen = inFile.read(inBuf, BUF_SIZE);
    if (readLen <= 0) break;

    for (int i = 0; i < readLen && ok; i++) {
      char ch = static_cast<char>(inBuf[i]);
reprocess:
      if (state == IN_ENTITY) {
        // Accumulate entity name chars: [a-zA-Z0-9#_]
        bool isNameCh = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '#' || ch == '_';
        if (isNameCh) {
          if (entityBufLen < 15) entityBuf[entityBufLen++] = ch;
        } else if (ch == ';') {
          // Entity reference ended properly.
          entityBuf[entityBufLen] = '\0';
          bool isXmlBuiltin = (entityBufLen > 0) &&
              (strcmp(entityBuf, "amp") == 0 || strcmp(entityBuf, "lt") == 0 ||
               strcmp(entityBuf, "gt") == 0  || strcmp(entityBuf, "quot") == 0 ||
               strcmp(entityBuf, "apos") == 0);
          bool isNumericRef = (entityBufLen > 0 && entityBuf[0] == '#');
          if (isXmlBuiltin || isNumericRef) {
            ok = writeOut('&');
            for (int j = 0; j < entityBufLen && ok; j++) ok = writeOut(entityBuf[j]);
            if (ok) ok = writeOut(';');
          } else {
            uint32_t cp = lookupHtmlEntity(entityBuf, entityBufLen);
            if (cp > 0) {
              char numRef[14];
              snprintf(numRef, sizeof(numRef), "&#%u;", static_cast<unsigned>(cp));
              writeStr(numRef);
            } else {
              writeStr("&amp;");
              for (int j = 0; j < entityBufLen && ok; j++) ok = writeOut(entityBuf[j]);
              if (ok) ok = writeOut(';');
            }
          }
          state = entityPrevState;
        } else {
          // Bare & — escape it and re-handle current char in restored state.
          writeStr("&amp;");
          for (int j = 0; j < entityBufLen && ok; j++) ok = writeOut(entityBuf[j]);
          state = entityPrevState;
          goto reprocess;
        }
      } else if (ch == '&' && !(state == IN_TAG && passthrough)) {
        // If an entity starts right at the beginning of an unquoted value
        // (e.g. attr=&amp;text), open the wrapping quote now so the entity
        // output lands inside the quotes.
        if (state == IN_TAG && attrState == AS_AFTER_EQ) {
          ok = writeOut('"');
          attrState = AS_IN_UNQUOTED;
        }
        entityPrevState = state;
        state           = IN_ENTITY;
        entityBufLen    = 0;
        entityBuf[0]    = '\0';
      } else if (state == TEXT) {
        ok = writeOut(static_cast<uint8_t>(ch));
        if (ch == '<') {
          state         = IN_TAG;
          tagNameLen    = 0;
          gatheringName = true;
          isClosingTag  = false;
          isVoid        = false;
          passthrough   = false;
          attrState     = AS_NONE;
          prevCh        = '<';
        }
      } else {  // IN_TAG
        if (ch == '>') {
          // Finalise tag name if we hit '>' before any whitespace (e.g. <br>)
          if (gatheringName && tagNameLen > 0) {
            tagName[tagNameLen] = '\0';
            if (!isClosingTag) isVoid = isVoidElement(tagName, tagNameLen);
            gatheringName = false;
          }
          // Close any open unquoted attribute value before the closing '>'.
          if (attrState == AS_IN_UNQUOTED) {
            ok = writeOut('"');
            attrState = AS_START;
          } else if (attrState == AS_AFTER_EQ) {
            // '=' with no value at all (edge case) — write empty string.
            writeStr("\"\"");
            attrState = AS_START;
          }
          // Insert '/' before '>' for void elements not already self-closed
          if (!passthrough && isVoid && prevCh != '/') {
            ok = writeOut('/');
          }
          if (ok) ok = writeOut('>');
          state     = TEXT;
          attrState = AS_NONE;
        } else if (!passthrough && !gatheringName && !isClosingTag) {
          // ---- Attribute sub-state machine ----
          switch (attrState) {
            case AS_START:
            case AS_IN_NAME:
              if (ch == '=') {
                ok = writeOut('=');
                attrState = AS_AFTER_EQ;
              } else if (isAttrSpace(ch)) {
                ok = writeOut(static_cast<uint8_t>(ch));
                attrState = AS_START;
              } else if (ch == '/') {
                ok = writeOut('/');
                attrState = AS_NONE;
                prevCh = '/';
              } else {
                ok = writeOut(static_cast<uint8_t>(ch));
                attrState = AS_IN_NAME;
              }
              break;
            case AS_AFTER_EQ:
              if (ch == '"') {
                ok = writeOut('"');
                attrState = AS_IN_DQUOTE;
              } else if (ch == '\'') {
                ok = writeOut('\'');
                attrState = AS_IN_SQUOTE;
              } else if (isAttrSpace(ch)) {
                // '=' followed by space — empty value.
                writeStr("\"\"");
                ok = ok && writeOut(static_cast<uint8_t>(ch));
                attrState = AS_START;
              } else {
                // Unquoted value starts here — insert opening '"'.
                ok = writeOut('"');
                ok = ok && writeOut(static_cast<uint8_t>(ch));
                attrState = AS_IN_UNQUOTED;
              }
              break;
            case AS_IN_DQUOTE:
              ok = writeOut(static_cast<uint8_t>(ch));
              if (ch == '"') attrState = AS_START;
              break;
            case AS_IN_SQUOTE:
              ok = writeOut(static_cast<uint8_t>(ch));
              if (ch == '\'') attrState = AS_START;
              break;
            case AS_IN_UNQUOTED:
              if (isAttrSpace(ch)) {
                // End of unquoted value — insert closing '"' then the space.
                ok = writeOut('"');
                ok = ok && writeOut(static_cast<uint8_t>(ch));
                attrState = AS_START;
              } else {
                ok = writeOut(static_cast<uint8_t>(ch));
              }
              break;
            default:  // AS_NONE — shouldn't happen here, fall through
              ok = writeOut(static_cast<uint8_t>(ch));
              break;
          }
          prevCh = ch;
        } else {
          // gatheringName or passthrough or closing tag: original logic.
          ok = writeOut(static_cast<uint8_t>(ch));
          if (gatheringName) {
            if (tagNameLen == 0 && ch == '/') {
              isClosingTag = true;
            } else if (tagNameLen == 0 && (ch == '!' || ch == '?')) {
              passthrough   = true;
              gatheringName = false;
            } else if (isAttrSpace(ch)) {
              tagName[tagNameLen] = '\0';
              if (!isClosingTag && tagNameLen > 0)
                isVoid = isVoidElement(tagName, tagNameLen);
              gatheringName = false;
              // Start attribute parsing only for non-closing, non-passthrough tags.
              if (!isClosingTag) attrState = AS_START;
            } else if (ch == '/') {
              tagName[tagNameLen] = '\0';
              if (!isClosingTag && tagNameLen > 0)
                isVoid = isVoidElement(tagName, tagNameLen);
              gatheringName = false;
            } else {
              if (tagNameLen < 31)
                tagName[tagNameLen++] = static_cast<char>(tolower(ch));
            }
          }
          prevCh = ch;
        }
      }
    }
  }

  // If file ended while still buffering an entity (bare & at EOF), escape it.
  if (ok && state == IN_ENTITY) {
    writeStr("&amp;");
    for (int j = 0; j < entityBufLen && ok; j++) ok = writeOut(entityBuf[j]);
  }

  // Flush remaining output bytes
  if (ok && outLen > 0) {
    ok = (outFile.write(outBuf, outLen) == outLen);
  }

  inFile.close();
  outFile.close();
  return ok;
}

}  // namespace (preprocessor helpers)

namespace {
constexpr uint8_t SECTION_FILE_VERSION = 15;
constexpr uint32_t HEADER_SIZE = sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(bool) + sizeof(uint8_t) +
                                 sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) +
                                 sizeof(uint8_t)+sizeof(bool)+ sizeof(bool) + sizeof(bool) + sizeof(bool) +
                                 sizeof(uint32_t);
}  // namespace

uint32_t Section::onPageComplete(std::unique_ptr<Page> page) {
  if (!page) {
    Serial.printf("[%lu] [SCT] Null page received at index %d\n", millis(), pageCount);
    return 0;
  }
  if (!file) {
    Serial.printf("[%lu] [SCT] File not open for writing page %d\n", millis(), pageCount);
    return 0;
  }

  const uint32_t position = file.position();
  if (!page->serialize(file)) {
    Serial.printf("[%lu] [SCT] Failed to serialize page %d\n", millis(), pageCount);
    return 0;
  }
  Serial.printf("[%lu] [SCT] Page %d processed\n", millis(), pageCount);

  pageCount++;
  return position;
}

void Section::writeSectionFileHeader(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                     const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                     const uint16_t viewportHeight, const bool hyphenationEnabled,const int8_t wordSpacing,
                                     const bool firstlineintented,
                                     const bool embeddedStyle, const bool chinesePunctFullWidth, const bool showImages) {
  if (!file) {
    Serial.printf("[%lu] [SCT] File not open for writing header\n", millis());
    return;
  }
  static_assert(HEADER_SIZE == sizeof(SECTION_FILE_VERSION) + sizeof(fontId) + sizeof(lineCompression) +
                                   sizeof(extraParagraphSpacing) + sizeof(paragraphAlignment) + sizeof(viewportWidth) +
                                   sizeof(viewportHeight) + sizeof(pageCount) + sizeof(hyphenationEnabled) +
                                   sizeof(firstlineintented) +sizeof(wordSpacing)+
                                   sizeof(embeddedStyle) + sizeof(chinesePunctFullWidth) + sizeof(showImages) + sizeof(uint32_t),
                "Header size mismatch");
  serialization::writePod(file, SECTION_FILE_VERSION);
  serialization::writePod(file, fontId);
  serialization::writePod(file, lineCompression);
  serialization::writePod(file, extraParagraphSpacing);
  serialization::writePod(file, paragraphAlignment);
  serialization::writePod(file, viewportWidth);
  serialization::writePod(file, viewportHeight);
  serialization::writePod(file, hyphenationEnabled);
  serialization::writePod(file, firstlineintented);
  serialization::writePod(file, wordSpacing);
  serialization::writePod(file, embeddedStyle);
  serialization::writePod(file, chinesePunctFullWidth);
  serialization::writePod(file, showImages);
  serialization::writePod(file, pageCount);  // Placeholder for page count (will be initially 0 when written)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for LUT offset
}

bool Section::loadSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                              const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                              const uint16_t viewportHeight, const bool hyphenationEnabled,const int8_t wordSpacing
                              , const bool firstlineintented, const bool embeddedStyle, const bool chinesePunctFullWidth, const bool showImages) {
  if (!SdMan.openFileForRead("SCT", filePath, file)) {
    return false;
  }

  // Match parameters
  {
    uint8_t version;
    serialization::readPod(file, version);
    if (version != SECTION_FILE_VERSION) {
      file.close();
      Serial.printf("[%lu] [SCT] Deserialization failed: Unknown version %u\n", millis(), version);
      clearCache();
      return false;
    }

    int fileFontId;
    uint16_t fileViewportWidth, fileViewportHeight;
    float fileLineCompression;
    bool fileExtraParagraphSpacing;
    uint8_t fileParagraphAlignment;
    bool fileHyphenationEnabled;
    bool fileFirstlineintented;
    int8_t fileWordSpacing;
    bool fileEmbeddedStyle;
    bool fileChinesePunctFullWidth;
    bool fileShowImages;
    serialization::readPod(file, fileFontId);
    serialization::readPod(file, fileLineCompression);
    serialization::readPod(file, fileExtraParagraphSpacing);
    serialization::readPod(file, fileParagraphAlignment);
    serialization::readPod(file, fileViewportWidth);
    serialization::readPod(file, fileViewportHeight);
    serialization::readPod(file, fileHyphenationEnabled);
    serialization::readPod(file, fileFirstlineintented);
    serialization::readPod(file, fileWordSpacing);
    serialization::readPod(file, fileEmbeddedStyle);
    serialization::readPod(file, fileChinesePunctFullWidth);
    serialization::readPod(file, fileShowImages);

    if (fontId != fileFontId || lineCompression != fileLineCompression ||
        extraParagraphSpacing != fileExtraParagraphSpacing || paragraphAlignment != fileParagraphAlignment ||
        viewportWidth != fileViewportWidth || viewportHeight != fileViewportHeight ||
        hyphenationEnabled != fileHyphenationEnabled || wordSpacing != fileWordSpacing||
        firstlineintented != fileFirstlineintented|| embeddedStyle != fileEmbeddedStyle ||
        chinesePunctFullWidth != fileChinesePunctFullWidth || showImages != fileShowImages) {
      file.close();
      Serial.printf("[%lu] [SCT] Deserialization failed: Parameters do not match\n", millis());
      clearCache();
      return false;
    }
  }

  serialization::readPod(file, pageCount);
  file.close();
  Serial.printf("[%lu] [SCT] Deserialization succeeded: %d pages\n", millis(), pageCount);
  return true;
}

// Your updated class method (assuming you are using the 'SD' object, which is a wrapper for a specific filesystem)
bool Section::clearCache() const {
  if (!SdMan.exists(filePath.c_str())) {
    Serial.printf("[%lu] [SCT] Cache does not exist, no action needed\n", millis());
    return true;
  }

  if (!SdMan.remove(filePath.c_str())) {
    Serial.printf("[%lu] [SCT] Failed to clear cache\n", millis());
    return false;
  }

  Serial.printf("[%lu] [SCT] Cache cleared successfully\n", millis());
  return true;
}

bool Section::createSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                const uint16_t viewportHeight, const bool hyphenationEnabled,const int8_t wordSpacing,
                                const bool firstlineintented, const bool embeddedStyle, const bool chinesePunctFullWidth, const bool showImages,
                                const std::function<void()>& popupFn) {
  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto tmpHtmlPath = epub->getCachePath() + "/.tmp_" + std::to_string(spineIndex) + ".html";

  // Create cache directory if it doesn't exist
  {
    const auto sectionsDir = epub->getCachePath() + "/sections";
    SdMan.mkdir(sectionsDir.c_str());
  }

  // Retry logic for SD card timing issues
  bool success = false;
  uint32_t fileSize = 0;
  for (int attempt = 0; attempt < 3 && !success; attempt++) {
    if (attempt > 0) {
      Serial.printf("[%lu] [SCT] Retrying stream (attempt %d)...\n", millis(), attempt + 1);
      delay(50);  // Brief delay before retry
    }

    // Remove any incomplete file from previous attempt before retrying
    if (SdMan.exists(tmpHtmlPath.c_str())) {
      SdMan.remove(tmpHtmlPath.c_str());
    }

    FsFile tmpHtml;
    if (!SdMan.openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) {
      continue;
    }
    success = epub->readItemContentsToStream(localPath, tmpHtml, 1024);
    fileSize = tmpHtml.size();
    tmpHtml.close();

    // If streaming failed, remove the incomplete file immediately
    if (!success && SdMan.exists(tmpHtmlPath.c_str())) {
      SdMan.remove(tmpHtmlPath.c_str());
      Serial.printf("[%lu] [SCT] Removed incomplete temp file after failed attempt\n", millis());
    }
  }

  if (!success) {
    Serial.printf("[%lu] [SCT] Failed to stream item contents to temp file after retries\n", millis());
    return false;
  }

  Serial.printf("[%lu] [SCT] Streamed temp HTML to %s (%d bytes)\n", millis(), tmpHtmlPath.c_str(), fileSize);

  // Preprocess HTML: make void elements self-closing so expat can parse
  // non-XHTML-compliant EPUB files (e.g. <br> → <br/>).
  const auto fixedHtmlPath = epub->getCachePath() + "/.fix_" + std::to_string(spineIndex) + ".html";
  const bool preprocessed = preprocessHtmlVoidElements(tmpHtmlPath, fixedHtmlPath);
  SdMan.remove(tmpHtmlPath.c_str());
  if (!preprocessed) {
    Serial.printf("[%lu] [SCT] HTML preprocessing failed\n", millis());
    return false;
  }
  Serial.printf("[%lu] [SCT] HTML void-elements fixed\n", millis());

  if (!SdMan.openFileForWrite("SCT", filePath, file)) {
    SdMan.remove(fixedHtmlPath.c_str());
    return false;
  }
  writeSectionFileHeader(fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
                         viewportHeight, hyphenationEnabled,wordSpacing,firstlineintented, embeddedStyle, chinesePunctFullWidth, showImages);
  std::vector<uint32_t> lut = {};
  // Derive the content base directory and image cache path prefix for the parser
  size_t lastSlash = localPath.find_last_of('/');
  std::string contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  std::string imageBasePath = epub->getCachePath() + "/img_" + std::to_string(spineIndex) + "_";

  ChapterHtmlSlimParser visitor(
      epub, fixedHtmlPath, renderer, fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
      viewportHeight, hyphenationEnabled,wordSpacing,firstlineintented,
      [this, &lut](std::unique_ptr<Page> page) { lut.emplace_back(this->onPageComplete(std::move(page))); },
      embeddedStyle,contentBase, imageBasePath,  popupFn, embeddedStyle ? epub->getCssParser() : nullptr,
      chinesePunctFullWidth, showImages);
  Hyphenator::setPreferredLanguage(epub->getLanguage());
  success = visitor.parseAndBuildPages();

  SdMan.remove(fixedHtmlPath.c_str());
  if (!success) {
    Serial.printf("[%lu] [SCT] Failed to parse XML and build pages\n", millis());
    file.close();
    SdMan.remove(filePath.c_str());
    return false;
  }

  const uint32_t lutOffset = file.position();
  bool hasFailedLutRecords = false;
  // Write LUT
  for (const uint32_t& pos : lut) {
    if (pos == 0) {
      hasFailedLutRecords = true;
      break;
    }
    serialization::writePod(file, pos);
  }

  if (hasFailedLutRecords) {
    Serial.printf("[%lu] [SCT] Failed to write LUT due to invalid page positions\n", millis());
    file.close();
    SdMan.remove(filePath.c_str());
    return false;
  }

  // Go back and write LUT offset
  file.seek(HEADER_SIZE - sizeof(uint32_t) - sizeof(pageCount));
  serialization::writePod(file, pageCount);
  serialization::writePod(file, lutOffset);
  file.close();
  return true;
}

std::unique_ptr<Page> Section::loadPageFromSectionFile() {
  if (!SdMan.openFileForRead("SCT", filePath, file)) {
    return nullptr;
  }

  file.seek(HEADER_SIZE - sizeof(uint32_t));
  uint32_t lutOffset;
  serialization::readPod(file, lutOffset);
  file.seek(lutOffset + sizeof(uint32_t) * currentPage);
  uint32_t pagePos;
  serialization::readPod(file, pagePos);
  file.seek(pagePos);

  auto page = Page::deserialize(file);
  file.close();
  return page;
}
