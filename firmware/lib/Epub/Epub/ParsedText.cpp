#include "ParsedText.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>
#include <vector>

#include "hyphenation/Hyphenator.h"


#include <list>  // 新增：list容器头文件
#include <string> // 新增：string头文件

constexpr int MAX_COST = std::numeric_limits<int>::max();

namespace {

// Soft hyphen byte pattern used throughout EPUBs (UTF-8 for U+00AD).
constexpr char SOFT_HYPHEN_UTF8[] = "\xC2\xAD";
constexpr size_t SOFT_HYPHEN_BYTES = 2;

bool containsSoftHyphen(const std::string& word) { return word.find(SOFT_HYPHEN_UTF8) != std::string::npos; }

// Removes every soft hyphen in-place so rendered glyphs match measured widths.
void stripSoftHyphensInPlace(std::string& word) {
  size_t pos = 0;
  while ((pos = word.find(SOFT_HYPHEN_UTF8, pos)) != std::string::npos) {
    word.erase(pos, SOFT_HYPHEN_BYTES);
  }
}

// Returns the rendered width for a word while ignoring soft hyphen glyphs and optionally appending a visible hyphen.
uint16_t measureWordWidth(const GfxRenderer& renderer, const int fontId, const std::string& word,
                          const EpdFontFamily::Style style, const bool appendHyphen = false) {
  const bool hasSoftHyphen = containsSoftHyphen(word);
  if (!hasSoftHyphen && !appendHyphen) {
    return renderer.getTextWidth(fontId, word.c_str(), style);
  }

  std::string sanitized = word;
  if (hasSoftHyphen) {
    stripSoftHyphensInPlace(sanitized);
  }
  if (appendHyphen) {
    sanitized.push_back('-');
  }
  return renderer.getTextWidth(fontId, sanitized.c_str(), style);
}

// 复刼LVGL的UTF-8解码（嵌入式环境通用，无依赖）
uint32_t utf8_next(const std::string& str, size_t& pos) {
    if(pos >= str.size()) return 0;

    unsigned char c = static_cast<unsigned char>(str[pos]);
    uint32_t cp = 0;
    size_t len = 0;

    // UTF-8解码规则（LVGL 同款）
    if(c < 0x80) { // 单字节（ASCII）
        cp = c;
        len = 1;
    } else if(c < 0xE0) { // 双字节
        cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(str[pos+1]) & 0x3F);
        len = 2;
    } else if(c < 0xF0) { // 三字节（中文/标点）
        cp = ((c & 0x0F) << 12) |
             ((static_cast<unsigned char>(str[pos+1]) & 0x3F) << 6) |
             (static_cast<unsigned char>(str[pos+2]) & 0x3F);
        len = 3;
    } else { // 四字节（极少用）
        cp = ((c & 0x07) << 18) |
             ((static_cast<unsigned char>(str[pos+1]) & 0x3F) << 12) |
             ((static_cast<unsigned char>(str[pos+2]) & 0x3F) << 6) |
             (static_cast<unsigned char>(str[pos+3]) & 0x3F);
        len = 4;
    }

    pos += len;
    return cp;
}
// 匹配中文标点（UTF-8）：。，！？；：、“”‘’（）【】《》，）】》”’
// 匹配禁止行首的中文标点（复刻LVGL逻辑）
bool isCJKLeadingPunctuation(const std::string& unit) {
    size_t pos = 0;
    uint32_t cp = utf8_next(unit, pos); // 解码为Unicode码点

    // 中文标点的Unicode码点（和LVGL一致）
    const uint32_t leading_puncts[] = {
        0x3002, // 。
        0xFF0C, // ，
        0xFF01, // ！
        0xFF1F, // ？
        0xFF1B, // ；
        0xFF1A, // ：
        0x3001, // 、
        0xFF09, // ）
        0x3011, // 】
        0x300B, // 》
        0x201D, // ”
        0x2019  // ’
    };

    // 遍历匹配码点
    for(size_t i = 0; i < sizeof(leading_puncts)/sizeof(leading_puncts[0]); i++) {
        if(cp == leading_puncts[i]) {
            return true;
        }
    }
    return false;
}

// ========== 中文标点全宽检测 ==========
// 判断一个 word 是否为中文全角标点（标准模式下需占用一个汉字宽度）
bool isChinesePunctForFullWidth(const std::string& word) {
    if (word.empty()) return false;
    size_t pos = 0;
    uint32_t cp = utf8_next(word, pos);
    if (cp == 0) return false;
    // 常见中文全角标点范围：
    // U+3000-U+303F: CJK Symbols and Punctuation（。、《》〈〉「」【】…）
    // U+FF00-U+FFEF: Halfwidth and Fullwidth Forms（，！？；：（））
    // U+2018-U+201D: Curly quotes（‘’“”）
    // U+2014: Em dash（—）  U+2013: En dash（–）
    // U+2026: Horizontal ellipsis（…）
    if ((cp >= 0x3000 && cp <= 0x303F) ||
        (cp >= 0xFF00 && cp <= 0xFFEF) ||
        (cp >= 0x2018 && cp <= 0x201D) ||
        cp == 0x2014 || cp == 0x2013 ||
        cp == 0x2026) {
        return true;
    }
    return false;
}
// ===========================================

// 判断是否为中文字符（单字/标点）
bool isCJKUnit(const std::string& unit) {
    if (unit.empty() || unit.size() > 3) return false;
    unsigned char firstByte = static_cast<unsigned char>(unit[0]);
    // UTF-8中文/标点首字节范围：0xE0~0xEF
    return firstByte >= 0xE0 && firstByte <= 0xEF;
}
// 辅助函数：获取list中指定索引的元素（适配嵌入式环境）
template <typename T>
const T& getListElement(const std::list<T>& lst, size_t index) {
    auto it = lst.begin();
    std::advance(it, index);
    return *it;
}

// 非const版本
template <typename T>
T& getListElement(std::list<T>& lst, size_t index) {
    auto it = lst.begin();
    std::advance(it, index);
    return *it;
}
// ========== 工具函数结束 ==========

}  // namespace

namespace {

// Returns true if the given UTF-8 codepoint is an invisible/zero-width character
// that should be stripped from words before layout.
bool isInvisibleCodepoint(uint32_t cp) {
  return cp == 0x00AD ||  // Soft Hyphen
         cp == 0x200B ||  // Zero Width Space
         cp == 0x200C ||  // ZWNJ
         cp == 0x200D ||  // ZWJ
         cp == 0x200E ||  // LTR Mark
         cp == 0x200F ||  // RTL Mark
         cp == 0x2028 ||  // Line Separator
         cp == 0x2029 ||  // Paragraph Separator
         cp == 0x202A ||  // LTR Embedding
         cp == 0x202C ||  // Pop Directional Formatting
         cp == 0x2060 ||  // Word Joiner
         cp == 0xFEFF;    // BOM / ZWNBSP
}

// Strip invisible Unicode characters from a UTF-8 string in-place.
// Keeps all visible characters including regular spaces.
void stripInvisibleChars(std::string& word) {
  std::string result;
  result.reserve(word.size());
  const auto* p = reinterpret_cast<const unsigned char*>(word.data());
  const auto* end = p + word.size();
  while (p < end) {
    const auto* start = p;
    uint32_t cp = 0;
    size_t bytes = 1;
    if (*p < 0x80) {
      cp = *p;
      bytes = 1;
    } else if ((*p & 0xE0) == 0xC0 && p + 1 < end) {
      cp = ((*p & 0x1F) << 6) | (*(p + 1) & 0x3F);
      bytes = 2;
    } else if ((*p & 0xF0) == 0xE0 && p + 2 < end) {
      cp = ((*p & 0x0F) << 12) | ((*(p + 1) & 0x3F) << 6) | (*(p + 2) & 0x3F);
      bytes = 3;
    } else if ((*p & 0xF8) == 0xF0 && p + 3 < end) {
      cp = ((*p & 0x07) << 18) | ((*(p + 1) & 0x3F) << 12) | ((*(p + 2) & 0x3F) << 6) | (*(p + 3) & 0x3F);
      bytes = 4;
    }
    p += bytes;
    if (!isInvisibleCodepoint(cp)) {
      result.append(reinterpret_cast<const char*>(start), bytes);
    }
  }
  word = std::move(result);
}

}  // anonymous namespace

void ParsedText::addWord(std::string word, const EpdFontFamily::Style fontStyle, const bool underline,
                         const bool attachToPrevious) {
  if (word.empty()) return;

  // Strip invisible Unicode characters (zero-width space, BOM, soft hyphen, etc.)
  stripInvisibleChars(word);
  if (word.empty()) return;

  words.push_back(std::move(word));
  EpdFontFamily::Style combinedStyle = fontStyle;
  if (underline) {
    combinedStyle = static_cast<EpdFontFamily::Style>(combinedStyle | EpdFontFamily::UNDERLINE);
  }
  wordStyles.push_back(combinedStyle);
  wordContinues.push_back(attachToPrevious);
}

// Consumes data to minimize memory usage
void ParsedText::layoutAndExtractLines(const GfxRenderer& renderer, const int fontId, const uint16_t viewportWidth,
                                       const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                                       const bool includeLastLine) {
  if (words.empty()) {
    return;
  }

  // Apply fixed transforms before any per-line layout work.
  applyParagraphIndent();
  //Serial.printf("首行缩进结束\n");

  const int pageWidth = viewportWidth;
  int spaceWidth = renderer.getSpaceWidth(fontId);

  // wordSpacing 直接作为字间距像素值：
  // - 布局阶段（computeLineBreaks）使用原始值，负值可让更多字上一行，左对齐不溢出
  // - Justify 渲染若出现 spareSpace<0，在 extractLine 中单独钳制防止溢出
  spaceWidth = wordSpacing;

  // ========== 可选：如果需要中文强制spaceWidth=0，取消注释以下逻辑 ==========
  // 定义中文判断辅助函数
  //auto isChineseText = [&]() -> bool {
  //  for (const auto& word : words) {
  //    for (char c : word) {
  //      // UTF-8中文的第一个字节范围：0xE4~0xE9（覆盖常用中文U+4E00~U+9FFF）
  //      if (c >= 0xE4 && c <= 0xE9) {
  //        return true;
  //      }
  //    }
  //  }
  //  return false;
  //};

  // 仅左对齐+中文时，强制spaceWidth=0（按需选择）
  //if (style == TextBlock::LEFT_ALIGN && isChineseText()) {
  //  spaceWidth = 0;
  //  Serial.printf("左对齐+中文文本，强制spaceWidth=0\n");
  //}



  auto wordWidths = calculateWordWidths(renderer, fontId);

  // Build indexed continues vector from the parallel list for O(1) access during layout
  std::vector<bool> continuesVec(wordContinues.begin(), wordContinues.end());

  std::vector<size_t> lineBreakIndices;
  if (hyphenationEnabled) {
    // Use greedy layout that can split words mid-loop when a hyphenated prefix fits.
    lineBreakIndices = computeHyphenatedLineBreaks(renderer, fontId, pageWidth, spaceWidth, wordWidths, continuesVec);
  } else {
    lineBreakIndices = computeLineBreaks(renderer, fontId, pageWidth, spaceWidth, wordWidths, continuesVec);
  }
  const size_t lineCount = includeLastLine ? lineBreakIndices.size() : lineBreakIndices.size() - 1;

  for (size_t i = 0; i < lineCount; ++i) {
    extractLine(i, pageWidth, spaceWidth, wordWidths, continuesVec, lineBreakIndices, processLine,renderer, fontId);
  }

  // After the first layout call, disable first-line indent so that subsequent
  // continuation calls (mid-paragraph splits in characterData) don't incorrectly
  // apply paragraph indentation to mid-paragraph text.
  firstlineintented = false;
}

std::vector<uint16_t> ParsedText::calculateWordWidths(const GfxRenderer& renderer, const int fontId) {
  const size_t totalWordCount = words.size();

  std::vector<uint16_t> wordWidths;
  wordWidths.reserve(totalWordCount);

  // 标准模式下汉字宽度（以“我”为参考）
  const int chineseCharWidth = chinesePunctFullWidth ? renderer.getTextWidth(fontId, "\xe6\x88\x91", EpdFontFamily::REGULAR) : 0;

  auto wordsIt = words.begin();
  auto wordStylesIt = wordStyles.begin();

  while (wordsIt != words.end()) {
    uint16_t w = measureWordWidth(renderer, fontId, *wordsIt, *wordStylesIt);
    // 标准模式：中文全角标点占满一个汉字宽度
    if (chinesePunctFullWidth && chineseCharWidth > 0 && w < static_cast<uint16_t>(chineseCharWidth)) {
      if (isChinesePunctForFullWidth(*wordsIt)) {
        w = static_cast<uint16_t>(chineseCharWidth);
      }
    }
    wordWidths.push_back(w);

    std::advance(wordsIt, 1);
    std::advance(wordStylesIt, 1);
  }

  return wordWidths;
}

std::vector<size_t> ParsedText::computeLineBreaks(const GfxRenderer& renderer, const int fontId, const int pageWidth,
                                                  const int spaceWidth, std::vector<uint16_t>& wordWidths,
                                                  std::vector<bool>& continuesVec) {
  if (words.empty()) {
    return {};
  }

  // Calculate first line indent (only for left/justified text without extra paragraph spacing)
  const int firstLineIndent =
      firstlineintented &&
              (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left)
          ? 2*renderer.getTextWidth(fontId,"我")
          : 0;

  // Ensure any word that would overflow even as the first entry on a line is split using fallback hyphenation.
  for (size_t i = 0; i < wordWidths.size(); ++i) {
    // First word needs to fit in reduced width if there's an indent
    const int effectiveWidth = i == 0 ? pageWidth - firstLineIndent : pageWidth;
    while (wordWidths[i] > effectiveWidth) {
      if (!hyphenateWordAtIndex(i, effectiveWidth, renderer, fontId, wordWidths, /*allowFallbackBreaks=*/true,
                                &continuesVec)) {
        break;
      }
    }
  }

  const size_t totalWordCount = words.size();

  // DP table to store the minimum badness (cost) of lines starting at index i
  std::vector<int> dp(totalWordCount);
  // 'ans[i]' stores the index 'j' of the *last word* in the optimal line starting at 'i'
  std::vector<size_t> ans(totalWordCount);

  // Base Case
  dp[totalWordCount - 1] = 0;
  ans[totalWordCount - 1] = totalWordCount - 1;

  for (int i = totalWordCount - 2; i >= 0; --i) {
    int currlen = 0;
    dp[i] = MAX_COST;

    // First line has reduced width due to text-indent
    const int effectivePageWidth = i == 0 ? pageWidth - firstLineIndent : pageWidth;

    for (size_t j = i; j < totalWordCount; ++j) {
      // Add space before word j, unless it's the first word on the line or a continuation
      const int gap = j > static_cast<size_t>(i) && !continuesVec[j] ? spaceWidth : 0;
      currlen += wordWidths[j] + gap;

      if (currlen > effectivePageWidth) {
        break;
      }

      // Cannot break after word j if the next word attaches to it (continuation group)
      if (j + 1 < totalWordCount && continuesVec[j + 1]) {
        continue;
      }

      int cost;
      if (j == totalWordCount - 1) {
        cost = 0;  // Last line
      } else {
        const int remainingSpace = effectivePageWidth - currlen;
        // Use long long for the square to prevent overflow
        const long long cost_ll = static_cast<long long>(remainingSpace) * remainingSpace + dp[j + 1];

        if (cost_ll > MAX_COST) {
          cost = MAX_COST;
        } else {
          cost = static_cast<int>(cost_ll);
        }
      }

      if (cost < dp[i]) {
        dp[i] = cost;
        ans[i] = j;  // j is the index of the last word in this optimal line
      }
    }

    // Handle oversized word: if no valid configuration found, force single-word line
    // This prevents cascade failure where one oversized word breaks all preceding words
    if (dp[i] == MAX_COST) {
      ans[i] = i;  // Just this word on its own line
      // Inherit cost from next word to allow subsequent words to find valid configurations
      if (i + 1 < static_cast<int>(totalWordCount)) {
        dp[i] = dp[i + 1];
      } else {
        dp[i] = 0;
      }
    }
  }

  // Stores the index of the word that starts the next line (last_word_index + 1)
  std::vector<size_t> lineBreakIndices;
  size_t currentWordIndex = 0;

  while (currentWordIndex < totalWordCount) {
    size_t nextBreakIndex = ans[currentWordIndex] + 1;

    // ========== 修复：标点避忌（适配list） ==========
    // 循环处理连续闭合标点（如 '" ），全部挤到上一行末
    while (nextBreakIndex < totalWordCount &&
           isCJKLeadingPunctuation(getListElement(words, nextBreakIndex))) {
        ++nextBreakIndex;
    }
    // ===========================================

    // Safety check: prevent infinite loop if nextBreakIndex doesn't advance
    if (nextBreakIndex <= currentWordIndex) {
      // Force advance by at least one word to avoid infinite loop
      nextBreakIndex = currentWordIndex + 1;
    }

    lineBreakIndices.push_back(nextBreakIndex);
    currentWordIndex = nextBreakIndex;
  }

  return lineBreakIndices;
}



void ParsedText::applyParagraphIndent() {
  // Only apply paragraph indent once per text block, even if layoutAndExtractLines
  // is called multiple times (e.g., during mid-paragraph splits in characterData).
  if (paragraphIndentApplied) {
    return;
  }
  paragraphIndentApplied = true;

  //Serial.printf("已进入此函数\n");
  if (blockStyle.alignment == CssTextAlign::Left && firstlineintented) {
    //Serial.printf("已进入\n");
    //words.front().insert(0, "\xe3\x80\x80\xe3\x80\x80"); // 两个全角空格，替代原来的1个窄空格
    //Serial.printf("首行缩进应用：%d\n", firstlineintented);
  }

  if (extraParagraphSpacing || words.empty()) {
    return;
  }

  if (firstlineintented) {
    // firstlineintented uses layout-based indent (firstLineIndent in extractLine()),
    // skip EmSpace insertion to avoid double indent
  } else if (blockStyle.textIndentDefined) {
    // CSS text-indent is explicitly set (even if 0) - don't use fallback EmSpace
    // The actual indent positioning is handled in extractLine()
  } else if (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left) {
    // No CSS text-indent defined - use EmSpace fallback for visual indent
    words.front().insert(0, "\xe2\x80\x83");
  }
}

// Builds break indices while opportunistically splitting the word that would overflow the current line.
std::vector<size_t> ParsedText::computeHyphenatedLineBreaks(const GfxRenderer& renderer, const int fontId,
                                                            const int pageWidth, const int spaceWidth,
                                                            std::vector<uint16_t>& wordWidths,
                                                            std::vector<bool>& continuesVec) {
  // Calculate first line indent (only for left/justified text without extra paragraph spacing)
  const int firstLineIndent =
      firstlineintented &&
              (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left)
          ? 2*renderer.getTextWidth(fontId,"我")
          : 0;

  std::vector<size_t> lineBreakIndices;
  size_t currentIndex = 0;
  bool isFirstLine = true;

  while (currentIndex < wordWidths.size()) {
    const size_t lineStart = currentIndex;
    int lineWidth = 0;

    // First line has reduced width due to text-indent
    const int effectivePageWidth = isFirstLine ? pageWidth - firstLineIndent : pageWidth;

    // Consume as many words as possible for current line, splitting when prefixes fit
    while (currentIndex < wordWidths.size()) {
      const bool isFirstWord = currentIndex == lineStart;
      const int spacing = isFirstWord || continuesVec[currentIndex] ? 0 : spaceWidth;
      const int candidateWidth = spacing + wordWidths[currentIndex];

      // Word fits on current line
      if (lineWidth + candidateWidth <= effectivePageWidth) {
        lineWidth += candidateWidth;
        ++currentIndex;
        continue;
      }

      // Word would overflow — try to split based on hyphenation points
      const int availableWidth = effectivePageWidth - lineWidth - spacing;
      const bool allowFallbackBreaks = isFirstWord;  // Only for first word on line

      if (availableWidth > 0 && hyphenateWordAtIndex(currentIndex, availableWidth, renderer, fontId, wordWidths,
                                                     allowFallbackBreaks, &continuesVec)) {
        // Prefix now fits; append it to this line and move to next line
        lineWidth += spacing + wordWidths[currentIndex];
        ++currentIndex;
        break;
      }

      // Could not split: force at least one word per line to avoid infinite loop
      if (currentIndex == lineStart) {
        lineWidth += candidateWidth;
        ++currentIndex;
      }
      break;
    }

    // Don't break before a continuation word (e.g., orphaned "?" after "question").
    // Backtrack to the start of the continuation group so the whole group moves to the next line.
    while (currentIndex > lineStart + 1 && currentIndex < wordWidths.size() && continuesVec[currentIndex]) {
      --currentIndex;
    }

    // ========== 修复：标点避忌（适配list） ==========
    // 循环处理连续闭合标点（如 '" ），全部挤到上一行末
    while (currentIndex < wordWidths.size() &&
           currentIndex > lineStart &&
           isCJKLeadingPunctuation(getListElement(words, currentIndex))) {
        ++currentIndex;
    }
    // ===========================================

    lineBreakIndices.push_back(currentIndex);
    isFirstLine = false;
  }

  return lineBreakIndices;
}



// Splits words[wordIndex] into prefix (adding a hyphen only when needed) and remainder when a legal breakpoint fits the
// available width.
bool ParsedText::hyphenateWordAtIndex(const size_t wordIndex, const int availableWidth, const GfxRenderer& renderer,
                                      const int fontId, std::vector<uint16_t>& wordWidths,
                                      const bool allowFallbackBreaks, std::vector<bool>* continuesVec) {
  // Guard against invalid indices or zero available width before attempting to split.
  if (availableWidth <= 0 || wordIndex >= words.size()) {
    return false;
  }

  // Get iterators to target word and style.
  auto wordIt = words.begin();
  auto styleIt = wordStyles.begin();
  std::advance(wordIt, wordIndex);
  std::advance(styleIt, wordIndex);

  const std::string& word = *wordIt;
  const auto style = *styleIt;

  // Collect candidate breakpoints (byte offsets and hyphen requirements).
  auto breakInfos = Hyphenator::breakOffsets(word, allowFallbackBreaks);
  if (breakInfos.empty()) {
    return false;
  }

  size_t chosenOffset = 0;
  int chosenWidth = -1;
  bool chosenNeedsHyphen = true;

  // Iterate over each legal breakpoint and retain the widest prefix that still fits.
  for (const auto& info : breakInfos) {
    const size_t offset = info.byteOffset;
    if (offset == 0 || offset >= word.size()) {
      continue;
    }

    const bool needsHyphen = info.requiresInsertedHyphen;
    const int prefixWidth = measureWordWidth(renderer, fontId, word.substr(0, offset), style, needsHyphen);
    if (prefixWidth > availableWidth || prefixWidth <= chosenWidth) {
      continue;  // Skip if too wide or not an improvement
    }

    chosenWidth = prefixWidth;
    chosenOffset = offset;
    chosenNeedsHyphen = needsHyphen;
  }

  if (chosenWidth < 0) {
    // No hyphenation point produced a prefix that fits in the remaining space.
    return false;
  }

  // Split the word at the selected breakpoint and append a hyphen if required.
  std::string remainder = word.substr(chosenOffset);
  wordIt->resize(chosenOffset);
  if (chosenNeedsHyphen) {
    wordIt->push_back('-');
  }

  // Insert the remainder word (with matching style and continuation flag) directly after the prefix.
  auto insertWordIt = std::next(wordIt);
  auto insertStyleIt = std::next(styleIt);
  words.insert(insertWordIt, remainder);
  wordStyles.insert(insertStyleIt, style);

  // The remainder inherits whatever continuation status the original word had with the word after it.
  // Find the continues entry for the original word and insert the remainder's entry after it.
  auto continuesIt = wordContinues.begin();
  std::advance(continuesIt, wordIndex);
  const bool originalContinuedToNext = *continuesIt;
  // The original word (now prefix) does NOT continue to remainder (hyphen separates them)
  *continuesIt = false;
  const auto insertContinuesIt = std::next(continuesIt);
  wordContinues.insert(insertContinuesIt, originalContinuedToNext);

  // Keep the indexed vector in sync if provided
  if (continuesVec) {
    (*continuesVec)[wordIndex] = false;
    continuesVec->insert(continuesVec->begin() + wordIndex + 1, originalContinuedToNext);
  }

  // Update cached widths to reflect the new prefix/remainder pairing.
  wordWidths[wordIndex] = static_cast<uint16_t>(chosenWidth);
  const uint16_t remainderWidth = measureWordWidth(renderer, fontId, remainder, style);
  wordWidths.insert(wordWidths.begin() + wordIndex + 1, remainderWidth);
  return true;
}

void ParsedText::extractLine(const size_t breakIndex, const int pageWidth, const int spaceWidth,
                             const std::vector<uint16_t>& wordWidths, const std::vector<bool>& continuesVec,
                             const std::vector<size_t>& lineBreakIndices,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,const GfxRenderer& renderer, int fontId) {
  const size_t lineBreak = lineBreakIndices[breakIndex];
  const size_t lastBreakAt = breakIndex > 0 ? lineBreakIndices[breakIndex - 1] : 0;
  const size_t lineWordCount = lineBreak - lastBreakAt;

  // Calculate first line indent (only for left/justified text without extra paragraph spacing)
  const bool isFirstLine = breakIndex == 0;

  const int firstLineIndent =
      isFirstLine && firstlineintented  &&
              (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left)
          ? 2*renderer.getTextWidth(fontId,"我") // Use double space width as a fallback indent for the first line
          : 0;

  // Calculate total word width for this line and count actual word gaps
  // (continuation words attach to previous word with no gap)
  int lineWordWidthSum = 0;
  size_t actualGapCount = 0;

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    lineWordWidthSum += wordWidths[lastBreakAt + wordIdx];
    // Count gaps: each word after the first creates a gap, unless it's a continuation
    if (wordIdx > 0 && !continuesVec[lastBreakAt + wordIdx]) {
      actualGapCount++;
    }
  }

  // Calculate spacing (account for indent reducing effective page width on first line)
  const int effectivePageWidth = pageWidth - firstLineIndent;
  const int spareSpace = effectivePageWidth - lineWordWidthSum;

  int spacing = spaceWidth;
  const bool isLastLine = breakIndex == lineBreakIndices.size() - 1;

  // For justified text, calculate spacing based on actual gap count
  // Use floor division so rendered width never exceeds pageWidth (C++ truncates toward zero).
  // The "remainder" pixels are distributed one-per-gap to the first justifyExtraGaps gaps,
  // ensuring the line fills exactly to pageWidth.
  int justifyExtraGaps = 0;
  if (blockStyle.alignment == CssTextAlign::Justify && !isLastLine && actualGapCount >= 1) {
    const int gaps = static_cast<int>(actualGapCount);
    spacing = spareSpace / gaps;
    // Floor division for negative dividend
    if (spareSpace < 0 && spareSpace % gaps != 0) {
      spacing -= 1;
    }
    // Number of gaps that get (spacing+1): distributes the remainder so the line
    // reaches exactly pageWidth. Always ≥ 0 by the floor-division property.
    justifyExtraGaps = spareSpace - spacing * gaps;
  }

  // Calculate initial x position (first line starts at indent for left/justified text)
  // Use int for xpos calculation to prevent uint16_t underflow when spacing is negative
  int xpos = firstLineIndent;
  if (blockStyle.alignment == CssTextAlign::Right) {
    xpos = spareSpace - static_cast<int>(actualGapCount) * spaceWidth;
  } else if (blockStyle.alignment == CssTextAlign::Center) {
    xpos = (spareSpace - static_cast<int>(actualGapCount) * spaceWidth) / 2;
  }

  // Pre-calculate X positions for words
  // Continuation words attach to the previous word with no space before them
  std::list<uint16_t> lineXPos;
  int justifyGapsUsed = 0;

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    const int currentWordWidth = static_cast<int>(wordWidths[lastBreakAt + wordIdx]);

    // Clamp xpos to non-negative before storing as uint16_t
    lineXPos.push_back(static_cast<uint16_t>(std::max(0, xpos)));

    // Add spacing after this word, unless the next word is a continuation
    const bool nextIsContinuation = wordIdx + 1 < lineWordCount && continuesVec[lastBreakAt + wordIdx + 1];
    if (!nextIsContinuation) {
      // Distribute remainder: first justifyExtraGaps gaps get (spacing+1) to fill exactly to pageWidth
      const int gapSpacing = (justifyGapsUsed < justifyExtraGaps) ? spacing + 1 : spacing;
      xpos += currentWordWidth + gapSpacing;
      justifyGapsUsed++;
    } else {
      xpos += currentWordWidth;
    }
  }

  // Iterators always start at the beginning as we are moving content with splice below
  auto wordEndIt = words.begin();
  auto wordStyleEndIt = wordStyles.begin();
  auto wordContinuesEndIt = wordContinues.begin();
  std::advance(wordEndIt, lineWordCount);
  std::advance(wordStyleEndIt, lineWordCount);
  std::advance(wordContinuesEndIt, lineWordCount);

  // *** CRITICAL STEP: CONSUME DATA USING SPLICE ***
  std::list<std::string> lineWords;
  lineWords.splice(lineWords.begin(), words, words.begin(), wordEndIt);
  std::list<EpdFontFamily::Style> lineWordStyles;
  lineWordStyles.splice(lineWordStyles.begin(), wordStyles, wordStyles.begin(), wordStyleEndIt);

  // Consume continues flags (not passed to TextBlock, but must be consumed to stay in sync)
  std::list<bool> lineContinues;
  lineContinues.splice(lineContinues.begin(), wordContinues, wordContinues.begin(), wordContinuesEndIt);

  for (auto& word : lineWords) {
    if (containsSoftHyphen(word)) {
      stripSoftHyphensInPlace(word);
    }
  }

  processLine(
      std::make_shared<TextBlock>(std::move(lineWords), std::move(lineXPos), std::move(lineWordStyles), blockStyle));
}
