#pragma once

// Unified UI text helpers for Murphy M4.
//
// Runtime TTF chrome uses three real, fixed raster sizes shared by the whole
// system and Native plugins (SMALL/UI_10/UI_12). Reader typography remains a
// separate independently-sized TTF face. This avoids bitmap-downscaling Reader
// glyphs for UI and keeps CJK strokes crisp at small sizes.
//
// Pure policy: M4UiTextPolicy.h (host-testable).
// Drawing helpers: this header (device / sim with GfxRenderer).

#include <GfxRenderer.h>
#include <EpdFontFamily.h>
#include <EpdFontLoader.h>
#include <Utf8.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "fontIds.h"
#include "util/M4FixedRuntimeUiFonts.h"
#include "util/M4UiRuntimePolicy.h"
#include "util/M4UiTextPolicy.h"

namespace M4UiText {

// Legacy generated epdfont chrome sizes.
inline int uiTtfSizeForLayout(int layoutFontId) {
  return layoutFontId == UI_10_FONT_ID ? 20 : 24;
}

inline bool selectedRuntimeTtf() {
  if (SETTINGS.fontFamily != CrossPointSettings::FONT_CUSTOM || SETTINGS.customFontFamily[0] == '\0') {
    return false;
  }
  std::string name = SETTINGS.customFontFamily;
  if (name.size() < 4) return false;
  std::string ext = name.substr(name.size() - 4);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return ext == ".ttf" || ext == ".ttc" || ext == ".otf" || ext == ".otc";
}

inline bool startsWithNoCase(const std::string& s, size_t pos, const char* literal) {
  if (!literal || pos > s.size()) return false;
  const size_t n = std::strlen(literal);
  if (pos + n > s.size()) return false;
  for (size_t i = 0; i < n; ++i) {
    const unsigned char a = static_cast<unsigned char>(s[pos + i]);
    const unsigned char b = static_cast<unsigned char>(literal[i]);
    if (std::tolower(a) != std::tolower(b)) return false;
  }
  return true;
}

// Provider introductions occasionally contain either literal or entity-escaped
// HTML line breaks. Normalize only the harmless break tags here so layout code
// never displays `<br/>` verbatim. Generic HTML stripping remains a provider
// responsibility.
inline std::string normalizeDisplayBreaks(const char* text) {
  std::string src = text ? text : "";
  std::string out;
  out.reserve(src.size());
  for (size_t i = 0; i < src.size();) {
    size_t skip = 0;
    if (startsWithNoCase(src, i, "<br>")) skip = 4;
    else if (startsWithNoCase(src, i, "<br/>")) skip = 5;
    else if (startsWithNoCase(src, i, "<br />")) skip = 6;
    else if (startsWithNoCase(src, i, "&lt;br&gt;")) skip = 10;
    else if (startsWithNoCase(src, i, "&lt;br/&gt;")) skip = 11;
    else if (startsWithNoCase(src, i, "&lt;br /&gt;")) skip = 12;

    if (skip != 0) {
      if (out.empty() || out.back() != '\n') out.push_back('\n');
      i += skip;
      continue;
    }
    out.push_back(src[i++]);
  }
  return out;
}

// SMALL/UI_10/UI_12 are authoritative chrome IDs. For runtime TTF, ensure they
// map to fixed native raster sizes (18/22/26px) shared by every system/plugin
// surface. Activity uiScale is intentionally NOT applied to runtime TTF text:
// arbitrary bitmap scaling is exactly what caused the poor small-font quality.
// Legacy epdfont keeps its historical bounded draw-time scaling behavior.
inline Face resolve(const GfxRenderer& renderer, int layoutFontId) {
  Face f;
  f.layoutFontId = (layoutFontId == 0) ? UI_12_FONT_ID : layoutFontId;
  f.fontId = f.layoutFontId;
  f.scale = 1.0f;

  if (selectedRuntimeTtf()) {
    auto& mutableRenderer = const_cast<GfxRenderer&>(renderer);
    if (M4FixedRuntimeUiFonts::ensure(mutableRenderer, SETTINGS.customFontFamily)) {
      const bool readerSized = layoutFontId == NOTOSANS_12_FONT_ID ||
                               layoutFontId == NOTOSANS_14_FONT_ID ||
                               layoutFontId == NOTOSANS_16_FONT_ID ||
                               layoutFontId == NOTOSANS_18_FONT_ID;
      if (readerSized) {
        const int readerFontId = SETTINGS.getReaderFontId();
        if (readerFontId != -1 && renderer.hasFont(readerFontId)) {
          f.fontId = readerFontId;
          return f;
        }
      }
      return f;
    }

    // Defensive fallback if a fixed UI face cannot be opened. Reader-sized
    // IDs still use the runtime TTF so CJK body text does not become '?'.
    const int readerFontId = SETTINGS.getReaderFontId();
    if (readerFontId != -1 && renderer.hasFont(readerFontId)) {
      f.fontId = readerFontId;
      if (layoutFontId != readerFontId &&
          (layoutFontId == NOTOSANS_12_FONT_ID || layoutFontId == NOTOSANS_14_FONT_ID ||
           layoutFontId == NOTOSANS_16_FONT_ID || layoutFontId == NOTOSANS_18_FONT_ID ||
           layoutFontId == SMALL_FONT_ID || layoutFontId == UI_10_FONT_ID ||
           layoutFontId == UI_12_FONT_ID)) {
        f.scale = renderer.scaleFontToMatch(readerFontId, f.layoutFontId);
      }
    }
    return f;
  }

  M4FixedRuntimeUiFonts::releaseIfDetached(renderer);
  f.scale *= M4UiRuntimePolicy::textScale();
  return f;
}

inline Face resolveForText(const GfxRenderer& renderer, int layoutFontId, const char* text,
                           EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  Face f = resolve(renderer, layoutFontId);
  if (SETTINGS.fontFamily != CrossPointSettings::FONT_CUSTOM ||
      strlen(SETTINGS.customFontFamily) == 0) {
    return f;
  }

  if (selectedRuntimeTtf()) {
    // Fixed runtime TTF chrome faces have full selected-font coverage.
    return f;
  }

  const char* safeText = text ? text : "";
  const int uiFont = EpdFontLoader::getBestFontId(
      SETTINGS.customFontFamily, uiTtfSizeForLayout(f.layoutFontId));
  if (uiFont != -1 && renderer.hasFont(uiFont) &&
      renderer.hasTextGlyphs(uiFont, safeText, style)) {
    f.fontId = uiFont;
  }
  return f;
}

inline int textWidth(const GfxRenderer& renderer, int layoutFontId, const char* text,
                     EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const Face f = resolveForText(renderer, layoutFontId, text, style);
  return renderer.getTextWidth(f.fontId, text ? text : "", style, f.scale);
}

inline std::string truncated(const GfxRenderer& renderer, int layoutFontId, const char* text, int maxWidth,
                             EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const Face f = resolveForText(renderer, layoutFontId, text, style);
  return renderer.truncatedText(f.fontId, text ? text : "", maxWidth, style, f.scale);
}

struct WrappedPage {
  std::vector<std::string> lines;
  size_t nextOffset = 0;
  bool hasMore = false;
};

// Wrap one page of already-normalized UTF-8 text. Offsets are byte offsets so
// callers can keep a tiny page stack without copying the remaining body.
inline WrappedPage wrapPage(const GfxRenderer& renderer, int layoutFontId,
                            const std::string& normalized, int maxWidth, int maxLines,
                            size_t startOffset = 0,
                            EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  WrappedPage page;
  page.nextOffset = std::min(startOffset, normalized.size());
  if (page.nextOffset >= normalized.size() || maxWidth <= 0 || maxLines <= 0) return page;

  const Face f = resolveForText(renderer, layoutFontId, normalized.c_str(), style);
  const char* begin = normalized.c_str();
  const char* end = begin + normalized.size();
  const char* p = begin + page.nextOffset;
  std::string current;

  auto widthOf = [&](const std::string& s) {
    return renderer.getTextWidth(f.fontId, s.c_str(), style, f.scale);
  };

  while (p < end) {
    if (*p == '\n' || *p == '\r') {
      while (p < end && (*p == '\n' || *p == '\r')) ++p;
      if (!current.empty()) {
        page.lines.push_back(current);
        current.clear();
      } else if (!page.lines.empty()) {
        page.lines.emplace_back();
      }
      if (static_cast<int>(page.lines.size()) >= maxLines) break;
      continue;
    }

    const char* cpBegin = p;
    const uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&p));
    if (cp == 0 || p <= cpBegin) break;
    const std::string glyphBytes(cpBegin, static_cast<size_t>(p - cpBegin));
    std::string candidate = current;
    candidate += glyphBytes;

    if (!current.empty() && widthOf(candidate) > maxWidth) {
      page.lines.push_back(current);
      current.clear();
      if (static_cast<int>(page.lines.size()) >= maxLines) {
        p = cpBegin;
        break;
      }
      current = glyphBytes;
      if (widthOf(current) > maxWidth) {
        current = renderer.truncatedText(f.fontId, current.c_str(), maxWidth, style, f.scale);
      }
    } else {
      current = std::move(candidate);
    }
  }

  if (static_cast<int>(page.lines.size()) < maxLines && !current.empty()) {
    page.lines.push_back(current);
  }
  page.nextOffset = static_cast<size_t>(p - begin);
  page.hasMore = page.nextOffset < normalized.size();
  return page;
}

// Width-aware UTF-8 wrapping used by XML/plugin title surfaces. Chinese text
// can break between any codepoints; Latin text is also allowed to break at a
// codepoint when a word is wider than the available e-ink column. The final
// visible line gets an ellipsis only when content remains undisplayed.
inline std::vector<std::string> wrapLines(const GfxRenderer& renderer, int layoutFontId,
                                          const char* text, int maxWidth, int maxLines,
                                          EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  if (!text || !*text || maxWidth <= 0 || maxLines <= 0) return {};
  const std::string normalized = normalizeDisplayBreaks(text);
  auto page = wrapPage(renderer, layoutFontId, normalized, maxWidth, maxLines, 0, style);

  if (page.hasMore && !page.lines.empty()) {
    const Face f = resolveForText(renderer, layoutFontId, normalized.c_str(), style);
    page.lines.back() = renderer.truncatedText(
        f.fontId, (page.lines.back() + "…").c_str(), maxWidth, style, f.scale);
  }
  return page.lines;
}

// List rows must use the metrics of the face that will actually be drawn.
inline int listLineHeight(const GfxRenderer& renderer, int layoutFontId) {
  const Face f = resolve(renderer, layoutFontId);
  const int advance = static_cast<int>(std::lround(renderer.getLineHeight(f.fontId) * f.scale));
  const int ascender = static_cast<int>(std::lround(renderer.getTextHeight(f.fontId) * f.scale));
  return std::max(1, std::max(advance, ascender));
}

inline int listSubtitleTop(const GfxRenderer& renderer, int layoutFontId, int titleTop = 4) {
  constexpr int kLegacyTop = 30;
  constexpr int kLineGap = 4;
  return std::max(kLegacyTop, titleTop + listLineHeight(renderer, layoutFontId) + kLineGap);
}

inline int listRowHeight(const GfxRenderer& renderer, int layoutFontId, int baseRowHeight,
                        bool hasSubtitle) {
  if (!hasSubtitle) return baseRowHeight;
  constexpr int kTitleTop = 4;
  constexpr int kBottomPadding = 4;
  const int subtitleTop = listSubtitleTop(renderer, layoutFontId, kTitleTop);
  return std::max(baseRowHeight, subtitleTop + listLineHeight(renderer, layoutFontId) + kBottomPadding);
}

inline void draw(const GfxRenderer& renderer, int layoutFontId, int x, int y, const char* text,
                 bool black = true, EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const Face f = resolveForText(renderer, layoutFontId, text, style);
  renderer.drawText(f.fontId, x, y, text ? text : "", black, style, f.scale);
}

// Ordered-dither UI text on the normal 1-bit framebuffer. This does not invoke
// the panel's multi-pass grayscale mode. Instead it uses the TTF's native 2-bit
// coverage as an anti-alias mask and a 4x4 Bayer pattern to approximate gray.
// `tone16`: 16=normal black, 12≈75% black, 10≈62%, 8≈50%.
// Use this for metadata/secondary chrome, not Reader body text.
inline void drawDithered(const GfxRenderer& renderer, int layoutFontId, int x, int y, const char* text,
                         uint8_t tone16 = 12,
                         EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  if (!text || !*text) return;
  if (tone16 >= 16) {
    draw(renderer, layoutFontId, x, y, text, true, style);
    return;
  }
  tone16 = std::max<uint8_t>(1, std::min<uint8_t>(16, tone16));

  const Face f = resolveForText(renderer, layoutFontId, text, style);
  if (f.scale < 0.999f || f.scale > 1.001f) {
    // Dithered path is intentionally native-size only. Legacy scaled faces use
    // the historical renderer to avoid a second resampling stage.
    renderer.drawText(f.fontId, x, y, text, true, style, f.scale);
    return;
  }

  const EpdFont* font = renderer.getFontPtr(f.fontId, style);
  if (!font) return;
  const EpdFontData* data = font->getData(style);
  if (!data) return;

  static constexpr uint8_t kBayer4[16] = {
      0, 8, 2, 10,
      12, 4, 14, 6,
      3, 11, 1, 9,
      15, 7, 13, 5,
  };

  int penX = x;
  const int baseline = y + data->ascender;
  const char* p = text;
  uint32_t cp = 0;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&p))) != 0) {
    const EpdGlyph* glyph = font->getGlyph(cp, style);
    if (!glyph) glyph = font->getGlyph('?', style);
    if (!glyph) continue;

    const uint8_t* bitmap = font->loadGlyphBitmap(glyph, nullptr, style);
    if (bitmap && glyph->width > 0 && glyph->height > 0) {
      for (int gy = 0; gy < glyph->height; ++gy) {
        const int sy = baseline - glyph->top + gy;
        for (int gx = 0; gx < glyph->width; ++gx) {
          const int pixel = gy * glyph->width + gx;
          uint8_t baseInk16 = 0;
          if (data->is2Bit) {
            const uint8_t raw = static_cast<uint8_t>(
                (bitmap[pixel / 4] >> ((3 - (pixel % 4)) * 2)) & 0x3u);
            const uint8_t level = static_cast<uint8_t>((3u - raw) & 0x3u);
            // Match the renderer's 2-bit convention: 0=solid ink, 3=white.
            baseInk16 = level == 0 ? 16 : (level == 1 ? 12 : (level == 2 ? 6 : 0));
          } else {
            const bool ink = ((bitmap[pixel / 8] >> (7 - (pixel % 8))) & 0x1u) != 0;
            baseInk16 = ink ? 16 : 0;
          }
          if (baseInk16 == 0) continue;

          const uint8_t ink16 = static_cast<uint8_t>((baseInk16 * tone16 + 8u) / 16u);
          const int sx = penX + glyph->left + gx;
          const uint8_t threshold = kBayer4[((sy & 3) << 2) | (sx & 3)];
          if (threshold < ink16) renderer.drawPixel(sx, sy, true);
        }
      }
    }
    penX += glyph->advanceX;
  }
}

inline void drawMuted(const GfxRenderer& renderer, int layoutFontId, int x, int y, const char* text,
                      EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  drawDithered(renderer, layoutFontId, x, y, text, 12, style);
}

// Centered on full screen width (same contract as GfxRenderer::drawCenteredText).
inline void drawCentered(const GfxRenderer& renderer, int layoutFontId, int y, const char* text,
                         bool black = true, EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const Face f = resolveForText(renderer, layoutFontId, text, style);
  const int w = renderer.getTextWidth(f.fontId, text ? text : "", style, f.scale);
  const int x = (renderer.getScreenWidth() - w) / 2;
  renderer.drawText(f.fontId, x, y, text ? text : "", black, style, f.scale);
}

// Draw a chrome label centered inside a touch target. GfxRenderer::drawText
// takes a top-left y coordinate (not a baseline), so center from actual metrics.
inline void drawCenteredInBox(const GfxRenderer& renderer, int layoutFontId, int x, int y, int width, int height,
                              const char* text, bool black = true,
                              EpdFontFamily::Style style = EpdFontFamily::REGULAR, int horizontalPadding = 12) {
  if (width <= 0 || height <= 0) return;
  const Face f = resolveForText(renderer, layoutFontId, text, style);
  const int maxWidth = std::max(1, width - 2 * std::max(0, horizontalPadding));
  const std::string label = renderer.truncatedText(f.fontId, text ? text : "", maxWidth, style, f.scale);
  const int textWidth = renderer.getTextWidth(f.fontId, label.c_str(), style, f.scale);
  const int textHeight = std::max(1, static_cast<int>(std::lround(renderer.getTextHeight(f.fontId) * f.scale)));
  const int textX = x + std::max(0, (width - textWidth) / 2);
  const int textY = y + std::max(0, (height - textHeight) / 2);
  renderer.drawText(f.fontId, textX, textY, label.c_str(), black, style, f.scale);
}

// Align list icons with the actual title/subtitle text block.
inline int listIconTop(const GfxRenderer& renderer, int layoutFontId, int rowHeight, bool hasSubtitle,
                       int iconSize, int titleTop = 4, int subtitleTop = 30) {
  if (rowHeight <= 0 || iconSize <= 0) return 0;
  if (!hasSubtitle) return std::max(0, (rowHeight - iconSize) / 2);
  const Face f = resolve(renderer, layoutFontId);
  const int lineHeight = std::max(1, static_cast<int>(std::lround(renderer.getTextHeight(f.fontId) * f.scale)));
  const int blockTop = titleTop;
  const int blockBottom = hasSubtitle ? subtitleTop + lineHeight : titleTop + lineHeight;
  const int blockCenter = blockTop + std::max(0, blockBottom - blockTop) / 2;
  return std::max(0, blockCenter - iconSize / 2);
}

// Chapter-list style row uses the stable UI chrome mapping; changing reader
// size no longer changes chapter/menu typography.
inline Face resolveChapterRow(const GfxRenderer& renderer, int chromeFontId) {
  return resolve(renderer, chromeFontId);
}

}  // namespace M4UiText
