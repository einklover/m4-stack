#pragma once

#include <GfxRenderer.h>

#include <vector>

#include "../../src/util/M4FontPolicy.h"

class EpdFontLoader {
 public:
  // Full SD /fonts rescan + promote reader IDs. Returns false when an explicit
  // custom font was selected but could not be loaded.
  static bool loadFontsFromSd(GfxRenderer& renderer);
  // No-op if loadFontsFromSd already completed this session; otherwise full scan.
  // Safe for TOC / menus that need full-CJK titles without paying rescan cost every open.
  static void ensureFontsFromSd(GfxRenderer& renderer);
  // True after at least one loadFontsFromSd finished (success or missing SD font).
  static bool fontsFromSdLoaded() { return sdFontsLoaded_; }
  static int getBestFontId(const char* familyName, int size);
  // Actual canonical promotion result for BOOT_SUMMARY (not header-only).
  static M4FontPolicy::LoadResult lastCanonicalLoadResult() { return lastCanonicalResult; }
  // Rebind SMALL/UI_10/UI_12 to the current 系统字号 without a full SD rescan.
  static void applySystemChrome(GfxRenderer& renderer);

 private:
  static std::vector<int> loadedCustomIds;
  static M4FontPolicy::LoadResult lastCanonicalResult;
  static bool sdFontsLoaded_;
};
