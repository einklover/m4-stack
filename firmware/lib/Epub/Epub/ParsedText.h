#pragma once

#include <EpdFontFamily.h>

#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"

class GfxRenderer;

class ParsedText {
  std::list<std::string> words;
  std::list<EpdFontFamily::Style> wordStyles;
  std::list<bool> wordContinues;  // true = word attaches to previous (no space before it)
  BlockStyle blockStyle;
  bool extraParagraphSpacing;
  bool hyphenationEnabled;
  bool firstlineintented;
  int8_t wordSpacing;
  bool chinesePunctFullWidth;  // true = render Chinese punctuation at full character width
  bool paragraphIndentApplied = false;  // Tracks whether applyParagraphIndent has already run

  void applyParagraphIndent();
  std::vector<size_t> computeLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth, int spaceWidth,
                                        std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec);
  std::vector<size_t> computeHyphenatedLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                                  int spaceWidth, std::vector<uint16_t>& wordWidths,
                                                  std::vector<bool>& continuesVec);
  bool hyphenateWordAtIndex(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                            std::vector<uint16_t>& wordWidths, bool allowFallbackBreaks,
                            std::vector<bool>* continuesVec = nullptr);
  void extractLine(size_t breakIndex, int pageWidth, int spaceWidth, const std::vector<uint16_t>& wordWidths,
                   const std::vector<bool>& continuesVec, const std::vector<size_t>& lineBreakIndices,
                   const std::function<void(std::shared_ptr<TextBlock>)>& processLine,const GfxRenderer& renderer, int fontId);
  std::vector<uint16_t> calculateWordWidths(const GfxRenderer& renderer, int fontId);


 public:
  explicit ParsedText(const bool extraParagraphSpacing, const bool hyphenationEnabled = false, const int8_t wordSpacing = 0,
                      const bool firstlineintented = false, const bool chinesePunctFullWidth = false,
                      const BlockStyle& blockStyle = BlockStyle())
      : blockStyle(blockStyle), extraParagraphSpacing(extraParagraphSpacing), hyphenationEnabled(hyphenationEnabled),
        wordSpacing(wordSpacing), firstlineintented(firstlineintented), chinesePunctFullWidth(chinesePunctFullWidth) {}
  ~ParsedText() = default;

  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false);
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  BlockStyle& getBlockStyle() { return blockStyle; }
  size_t size() const { return words.size(); }
  bool isEmpty() const { return words.empty(); }
  // Returns true if the block contains no visible content (empty or only whitespace/invisible characters).
  // Used to treat paragraphs like <p>&#160;</p> as blank and suppress their rendering.
  // Handles both ASCII and Unicode whitespace/invisible characters.
  bool isEffectivelyEmpty() const {
    if (words.empty()) return true;
    for (const auto& word : words) {
      const auto* p = reinterpret_cast<const unsigned char*>(word.data());
      const auto* end = p + word.size();
      while (p < end) {
        uint32_t cp = 0;
        if (*p < 0x80) {
          cp = *p++;
        } else if ((*p & 0xE0) == 0xC0 && p + 1 < end) {
          cp = ((*p & 0x1F) << 6) | (*(p + 1) & 0x3F);
          p += 2;
        } else if ((*p & 0xF0) == 0xE0 && p + 2 < end) {
          cp = ((*p & 0x0F) << 12) | ((*(p + 1) & 0x3F) << 6) | (*(p + 2) & 0x3F);
          p += 3;
        } else if ((*p & 0xF8) == 0xF0 && p + 3 < end) {
          cp = ((*p & 0x07) << 18) | ((*(p + 1) & 0x3F) << 12) | ((*(p + 2) & 0x3F) << 6) | (*(p + 3) & 0x3F);
          p += 4;
        } else {
          p++;
          continue;
        }
        // Check if this codepoint is a visible character
        if (cp != ' ' && cp != '\t' && cp != '\n' && cp != '\r' &&
            cp != 0x00A0 &&   // NBSP
            cp != 0x00AD &&   // Soft Hyphen
            cp != 0x200B &&   // Zero Width Space
            cp != 0x200C &&   // ZWNJ
            cp != 0x200D &&   // ZWJ
            cp != 0x200E && cp != 0x200F &&  // Directional marks
            cp != 0x2028 && cp != 0x2029 &&  // Line/Paragraph separator
            cp != 0x202A && cp != 0x202C &&  // Directional formatting
            cp != 0x2003 &&   // EmSpace (used for indent)
            cp != 0x2060 &&   // Word Joiner
            cp != 0x3000 &&   // Fullwidth Space
            cp != 0xFEFF) {   // BOM
          return false;  // Found a visible character
        }
      }
    }
    return true;
  }
  void layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                             bool includeLastLine = true);
};