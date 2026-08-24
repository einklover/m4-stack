#pragma once

#include <EpdFontFamily.h>
#include <GfxRenderer.h>

#include "../fontIds.h"

// System chrome fonts are permanently bound to the builtin 15x16 1-bit
// native-grid face registered at boot (SMALL / UI_10 / UI_12).
//
// A user-selected Reader font (runtime TTF/OTF/TTC/OTC or legacy epdfont) must
// NEVER replace those IDs, and reader pixel-size scaling must never wrap them.
// Earlier builds promoted complete custom faces onto the chrome IDs via
// M4FixedRuntimeUiFonts::ensure(); that leaked reader-face metrics into
// settings/menu/status layout and made Reader Settings tiny and overlapped
// after a font switch.
//
// Custom faces affect only reader/content hash IDs (and the NOTOSANS_* reader
// bind path). This header now exists only to restore builtin chrome if a stale
// session somehow left custom pointers mapped, and to document the contract.
namespace M4FixedRuntimeUiFonts {

struct Originals {
  bool captured = false;
  const EpdFont* smallRegular = nullptr;
  const EpdFont* ui10Regular = nullptr;
  const EpdFont* ui10Bold = nullptr;
  const EpdFont* ui12Regular = nullptr;
  const EpdFont* ui12Bold = nullptr;
};

inline Originals& originals() {
  static Originals o;
  return o;
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

inline void restore(GfxRenderer& renderer) {
  Originals& o = originals();
  if (!o.captured) captureOriginals(renderer);
  if (!o.captured) return;
  if (o.smallRegular) renderer.replaceFont(SMALL_FONT_ID, EpdFontFamily(o.smallRegular));
  if (o.ui10Regular) renderer.replaceFont(UI_10_FONT_ID, EpdFontFamily(o.ui10Regular, o.ui10Bold));
  if (o.ui12Regular) renderer.replaceFont(UI_12_FONT_ID, EpdFontFamily(o.ui12Regular, o.ui12Bold));
}

// Product rule: never promote a custom Reader face onto chrome IDs.
// Always restore builtins and report that custom chrome is not active.
inline bool ensure(GfxRenderer& renderer, const char* /*familyName*/) {
  restore(renderer);
  return false;
}

inline void releaseIfDetached(const GfxRenderer& /*renderer*/) {}

inline void invalidateFamilyDecision(const char* /*familyName*/ = nullptr) {}

// Chrome promotion of selected Reader fonts is permanently disabled on M4.
inline constexpr bool kAllowCustomChromePromotion = false;

}  // namespace M4FixedRuntimeUiFonts
