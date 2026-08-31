#pragma once

#include <EpdFontLoader.h>
#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <CenterKernelEpdFont.h>

#include "../fontIds.h"

extern const uint8_t m4_center_kernel_16x16_bin_start[] asm("_binary_src_fontdata_m4_center_kernel_16x16_bin_start");
extern const uint8_t m4_center_kernel_16x16_bin_end[] asm("_binary_src_fontdata_m4_center_kernel_16x16_bin_end");

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
// bind path). This header repairs stale chrome mappings through the current
// system-tier binder and retains boot-face fallback safety.
namespace M4FixedRuntimeUiFonts {

// Keep the original built-in UI faces addressable after a reader TTF replaces
// the public UI IDs. These IDs are private to the renderer and deliberately do
// not enter the generated font-id list.
inline constexpr int kSystemSmallFontId = 0x4D345301;
inline constexpr int kSystemUi10FontId = 0x4D345302;
inline constexpr int kSystemUi12FontId = 0x4D345303;
inline constexpr int kHubTitleFontId = 0x4D345310;
inline constexpr int kHubCategoryFontId = 0x4D345311;
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

inline int systemFontId(int layoutFontId) {
  if (layoutFontId == SMALL_FONT_ID) return kSystemSmallFontId;
  if (layoutFontId == UI_10_FONT_ID) return kSystemUi10FontId;
  return kSystemUi12FontId;
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

inline void mapSystemFaces(GfxRenderer& renderer) {
  const Originals& o = originals();
  if (o.smallRegular) renderer.replaceFont(kSystemSmallFontId, EpdFontFamily(o.smallRegular));
  if (o.ui10Regular) renderer.replaceFont(kSystemUi10FontId, EpdFontFamily(o.ui10Regular, o.ui10Bold));
  if (o.ui12Regular) renderer.replaceFont(kSystemUi12FontId, EpdFontFamily(o.ui12Regular, o.ui12Bold));
}

inline void ensureSystemFaces(GfxRenderer& renderer) {
  captureOriginals(renderer);
  mapSystemFaces(renderer);
}

inline void ensureHubFaces(GfxRenderer& renderer) {
  static bool done = false;
  if (done) return;
  static bool inited = false;
  static CenterKernelEpdFont hubTitleFont(m4_center_kernel_16x16_bin_start,
      static_cast<size_t>(m4_center_kernel_16x16_bin_end - m4_center_kernel_16x16_bin_start), 20);
  static CenterKernelEpdFont hubCategoryFont(m4_center_kernel_16x16_bin_start,
      static_cast<size_t>(m4_center_kernel_16x16_bin_end - m4_center_kernel_16x16_bin_start), 24);
  if (!inited) {
    // Ensure the fonts are constructed with correct pixel size; setPixelSize is redundant as ctor already sets it,
    // but keep for clarity if policy changes.
    hubTitleFont.setPixelSize(20);
    hubCategoryFont.setPixelSize(24);
    inited = true;
  }
  renderer.replaceFont(kHubTitleFontId, EpdFontFamily(&hubTitleFont));
  renderer.replaceFont(kHubCategoryFontId, EpdFontFamily(&hubCategoryFont));
  done = true;
}
inline void restore(GfxRenderer& renderer) {
  Originals& o = originals();
  if (!o.captured) captureOriginals(renderer);
  if (!o.captured) return;
  if (o.smallRegular) renderer.replaceFont(SMALL_FONT_ID, EpdFontFamily(o.smallRegular));
  if (o.ui10Regular) renderer.replaceFont(UI_10_FONT_ID, EpdFontFamily(o.ui10Regular, o.ui10Bold));
  if (o.ui12Regular) renderer.replaceFont(UI_12_FONT_ID, EpdFontFamily(o.ui12Regular, o.ui12Bold));
}

// Product rule: never promote a custom Reader face onto chrome IDs. Reapply
// the current system tier first; only use boot-captured faces as the fallback
// when the CenterKernel chrome blob is unavailable.
inline bool ensure(GfxRenderer& renderer, const char* /*familyName*/) {
  if (EpdFontLoader::applySystemChrome(renderer)) return false;
  restore(renderer);
  return false;
}

inline void releaseIfDetached(const GfxRenderer& /*renderer*/) {}

inline void invalidateFamilyDecision(const char* /*familyName*/ = nullptr) {}

// Chrome promotion of selected Reader fonts is permanently disabled on M4.
inline constexpr bool kAllowCustomChromePromotion = false;

}  // namespace M4FixedRuntimeUiFonts
