#pragma once

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <TtfEpdFont.h>

#include <cstdint>
#include <memory>
#include <string>

#include "../I18n.h"
#include "../fontIds.h"
#include "M4RuntimeUiFontPolicy.h"

// Fixed-size runtime-TTF faces shared by system chrome and Native plugins.
//
// Reader text keeps its independently selected raster size (for example 33px).
// UI chrome never bitmap-downscales that Reader face. Instead, three small
// TtfEpdFont instances are rasterized natively at 18/22/26px and mapped to the
// stable SMALL/UI_10/UI_12 IDs. All system/plugin call sites therefore reuse the
// same faces rather than creating arbitrary sizes.
//
// A user font is allowed to replace system chrome only when its cmap covers all
// strings in the active firmware language. Otherwise the selected TTF remains
// the Reader face while the compact built-in CJK UI subset stays authoritative.
// This prevents partial/Latin fonts from turning settings labels into '?'.
namespace M4FixedRuntimeUiFonts {

struct State {
  std::string family;
  std::unique_ptr<TtfEpdFont> small;
  std::unique_ptr<TtfEpdFont> ui10;
  std::unique_ptr<TtfEpdFont> ui12;
};

struct Originals {
  bool captured = false;
  const EpdFont* smallRegular = nullptr;
  const EpdFont* ui10Regular = nullptr;
  const EpdFont* ui10Bold = nullptr;
  const EpdFont* ui12Regular = nullptr;
  const EpdFont* ui12Bold = nullptr;
};

inline State& state() {
  static State s;
  return s;
}

inline Originals& originals() {
  static Originals o;
  return o;
}

inline std::string& rejectedFamily() {
  static std::string family;
  return family;
}

inline void captureOriginals(const GfxRenderer& renderer) {
  Originals& o = originals();
  if (o.captured) return;
  o.smallRegular = renderer.getFontPtr(SMALL_FONT_ID, EpdFontFamily::REGULAR);
  o.ui10Regular = renderer.getFontPtr(UI_10_FONT_ID, EpdFontFamily::REGULAR);
  o.ui10Bold = renderer.getFontPtr(UI_10_FONT_ID, EpdFontFamily::BOLD);
  o.ui12Regular = renderer.getFontPtr(UI_12_FONT_ID, EpdFontFamily::REGULAR);
  o.ui12Bold = renderer.getFontPtr(UI_12_FONT_ID, EpdFontFamily::BOLD);
  o.captured = o.smallRegular || o.ui10Regular || o.ui12Regular;
}

inline uint32_t nextUtf8(const char*& p) {
  const auto* s = reinterpret_cast<const uint8_t*>(p);
  if (!s || *s == 0) return 0;
  uint32_t cp = 0xFFFD;
  size_t n = 1;
  if (s[0] < 0x80) {
    cp = s[0];
  } else if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
    cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
    n = 2;
  } else if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
    cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    n = 3;
  } else if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 &&
             (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
    cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) |
         ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    n = 4;
  }
  p += n;
  return cp;
}

inline bool coversText(const TtfEpdFont& face, const char* text) {
  if (!text) return true;
  const char* p = text;
  while (*p) {
    const uint32_t cp = nextUtf8(p);
    if (cp == 0 || cp == '\n' || cp == '\r' || cp == '\t') continue;
    if (!face.hasCodepoint(cp)) return false;
  }
  return true;
}

inline bool coversActiveUi(const TtfEpdFont& face) {
  // I18n is the authoritative shipped system vocabulary. Provider/book strings
  // are dynamic and may fall back independently, but a custom font that cannot
  // render the settings/navigation vocabulary must never replace system chrome.
  for (uint16_t i = 0; i < static_cast<uint16_t>(Str::COUNT); ++i) {
    if (!coversText(face, L(static_cast<Str>(i)))) return false;
  }
  return true;
}

inline std::unique_ptr<TtfEpdFont> makeFace(const String& path, int px, uint16_t slots, size_t budget) {
  auto face = std::make_unique<TtfEpdFont>(path, static_cast<uint16_t>(px), slots, budget);
  if (!face || !face->valid()) return {};
  return face;
}

inline void mapFaces(GfxRenderer& renderer, State& s) {
  if (s.small) renderer.replaceFont(SMALL_FONT_ID, EpdFontFamily(s.small.get()));
  if (s.ui10) renderer.replaceFont(UI_10_FONT_ID, EpdFontFamily(s.ui10.get()));
  if (s.ui12) renderer.replaceFont(UI_12_FONT_ID, EpdFontFamily(s.ui12.get()));
}

inline void restore(GfxRenderer& renderer) {
  Originals& o = originals();
  if (o.captured) {
    if (o.smallRegular) renderer.replaceFont(SMALL_FONT_ID, EpdFontFamily(o.smallRegular));
    if (o.ui10Regular) renderer.replaceFont(UI_10_FONT_ID, EpdFontFamily(o.ui10Regular, o.ui10Bold));
    if (o.ui12Regular) renderer.replaceFont(UI_12_FONT_ID, EpdFontFamily(o.ui12Regular, o.ui12Bold));
  }
  state() = {};
}

inline void invalidateFamilyDecision(const char* familyName = nullptr) {
  if (!familyName || rejectedFamily() == familyName) rejectedFamily().clear();
}

inline bool ensure(GfxRenderer& renderer, const char* familyName) {
  if (!familyName || !*familyName) return false;
  State& s = state();

  if (s.family == familyName && s.small && s.ui10 && s.ui12) {
    if (renderer.getFontPtr(SMALL_FONT_ID) != s.small.get() ||
        renderer.getFontPtr(UI_10_FONT_ID) != s.ui10.get() ||
        renderer.getFontPtr(UI_12_FONT_ID) != s.ui12.get()) {
      mapFaces(renderer, s);
    }
    return true;
  }
  if (rejectedFamily() == familyName) return false;

  captureOriginals(renderer);
  const String path = String("/FONT/") + familyName;

  // Build the replacement set first so a failed SD/font parse never leaves the
  // renderer with dangling or partially replaced mappings.
  auto small = makeFace(path, M4RuntimeUiFontPolicy::kSmallBasePx, 64, 32u * 1024u);
  auto ui10 = makeFace(path, M4RuntimeUiFontPolicy::kUi10BasePx, 160, 96u * 1024u);
  auto ui12 = makeFace(path, M4RuntimeUiFontPolicy::kUi12BasePx, 128, 80u * 1024u);
  if (!small || !ui10 || !ui12) {
    rejectedFamily() = familyName;
    return false;
  }

  // One cmap is shared conceptually by all three sizes, so checking a single
  // face is enough. Refuse chrome promotion if even one active UI codepoint is
  // absent; the reader can still use the custom font normally.
  if (!coversActiveUi(*ui10)) {
    rejectedFamily() = familyName;
    restore(renderer);
    return false;
  }

  State next;
  next.family = familyName;
  next.small = std::move(small);
  next.ui10 = std::move(ui10);
  next.ui12 = std::move(ui12);

  rejectedFamily().clear();
  s = std::move(next);
  mapFaces(renderer, s);
  return true;
}

inline void releaseIfDetached(const GfxRenderer& renderer) {
  State& s = state();
  if (s.family.empty()) return;

  const bool stillMapped = renderer.getFontPtr(SMALL_FONT_ID) == s.small.get() ||
                           renderer.getFontPtr(UI_10_FONT_ID) == s.ui10.get() ||
                           renderer.getFontPtr(UI_12_FONT_ID) == s.ui12.get();
  if (!stillMapped) s = {};
}

}  // namespace M4FixedRuntimeUiFonts