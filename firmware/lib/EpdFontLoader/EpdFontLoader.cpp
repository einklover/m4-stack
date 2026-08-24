#include "EpdFontLoader.h"

#include <HardwareSerial.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(ESP32)
#include <esp_heap_caps.h>
#endif

#include "../../src/CrossPointSettings.h"
#include "../../src/fontIds.h"
#include "../../src/managers/FontManager.h"
#include "../../src/util/M4FontPolicy.h"
#ifdef CROSSPOINT_MURPHY_M4
#include "../EpdFont/ScaledEpdFont.h"
#include "../../src/util/M4FixedRuntimeUiFonts.h"
#endif

std::vector<int> EpdFontLoader::loadedCustomIds;
M4FontPolicy::LoadResult EpdFontLoader::lastCanonicalResult = M4FontPolicy::LoadResult::NotAttempted;
bool EpdFontLoader::sdFontsLoaded_ = false;

namespace {
int hashFontId(const char* familyName, int size) {
  std::string key = std::string(familyName) + "-" + std::to_string(size);
  uint32_t hash = 5381;
  for (char c : key) hash = ((hash << 5) + hash) + static_cast<uint8_t>(c);
  return static_cast<int>(hash);
}

bool isRuntimeTtfFamily(const std::string& familyName) {
  if (familyName.size() < 4) return false;
  std::string suffix = familyName.substr(familyName.size() - 4);
  std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  // Keep this in sync with FontManager::isRuntimeFontName(). Collections and
  // OpenType/CFF faces are the same streamed runtime backend as standalone TTF.
  // Misclassifying .otf/.ttc/.otc as a legacy epdfont makes the loader create
  // six independent reader sizes, each with its own parser/cache budget.
  return suffix == ".ttf" || suffix == ".ttc" || suffix == ".otf" || suffix == ".otc";
}

void logFontHeap(const char* stage) {
#if defined(ESP32)
  Serial.printf("[M4-FONT-HEAP] stage=%s internal_free=%u internal_largest=%u psram_free=%u\n",
                stage ? stage : "?",
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
#else
  (void)stage;
#endif
}

// Runtime sfnt faces are expensive (stream + cmap + cache metadata). Reuse the
// exact current reader face across settings/layout reloads instead of recreating
// it whenever orientation or a non-font setting invalidates pagination.
std::string activeRuntimeTtfFamily;
int activeRuntimeTtfSize = -1;

#ifdef CROSSPOINT_MURPHY_M4
// One borrowed source face plus one reusable scaler gives system reader text
// exact arbitrary pixel sizes without creating a 12/16/18 artifact matrix.
ScaledEpdFont scaledSystemReader;
const EpdFont* compactSystemReader = nullptr;

void captureCompactSystemReader(const GfxRenderer& renderer) {
  if (compactSystemReader) return;
  const EpdFont* candidate = renderer.getFontPtr(NOTOSANS_16_FONT_ID);
  if (candidate && candidate != &scaledSystemReader) compactSystemReader = candidate;
}

void logFontMap(const GfxRenderer& renderer, const char* stage, int readerId) {
  Serial.printf("[M4-FONT-MAP] stage=%s reader_id=%d reader=%p small_id=%d small=%p ui10_id=%d ui10=%p ui12_id=%d ui12=%p\n",
                stage ? stage : "?", readerId,
                static_cast<const void*>(readerId == -1 ? nullptr : renderer.getFontPtr(readerId)),
                SMALL_FONT_ID, static_cast<const void*>(renderer.getFontPtr(SMALL_FONT_ID)),
                UI_10_FONT_ID, static_cast<const void*>(renderer.getFontPtr(UI_10_FONT_ID)),
                UI_12_FONT_ID, static_cast<const void*>(renderer.getFontPtr(UI_12_FONT_ID)));
}
#endif

bool insertCustomFamily(GfxRenderer& renderer, const char* familyName, int size) {
  EpdFontFamily* family = FontManager::getInstance().getCustomFontFamily(familyName, size);
  if (!family) {
    Serial.printf("[FontLoader] Failed to load '%s' size %d\n", familyName, size);
    char line[192];
    snprintf(line, sizeof(line), "insert_fail family=%s size=%d", familyName ? familyName : "", size);
    FontManager::appendFontDiagnostic(line);
    return false;
  }
  const int id = hashFontId(familyName, size);
  renderer.insertFont(id, *family);
  Serial.printf("[FontLoader] Inserted custom font '%s' size %d id=%d\n", familyName, size, id);
  char line[192];
  snprintf(line, sizeof(line), "insert_ok family=%s size=%d id=%d", familyName ? familyName : "", size, id);
  FontManager::appendFontDiagnostic(line);
  return true;
}

int loadAndInsertCustom(GfxRenderer& renderer, const char* familyName, int size, std::vector<int>& outIds) {
  if (!insertCustomFamily(renderer, familyName, size)) return -1;
  const int id = hashFontId(familyName, size);
  outIds.push_back(id);
  return id;
}

void promoteToReaderIds(GfxRenderer& renderer, const char* familyName, int size) {
  EpdFontFamily* family = FontManager::getInstance().getCustomFontFamily(familyName, size);
  if (!family) return;
  // The release epdfont is a single fixed ~16 px face. Promote it only to
  // reader/content IDs. Replacing UI_10/UI_12/SMALL with the same large face
  // makes system menus overlap because those layouts expect compact metrics.
  renderer.replaceFont(NOTOSANS_12_FONT_ID, *family);
  renderer.replaceFont(NOTOSANS_14_FONT_ID, *family);
  renderer.replaceFont(NOTOSANS_16_FONT_ID, *family);
  renderer.replaceFont(NOTOSANS_18_FONT_ID, *family);
  Serial.printf("[M4-FONT] Promoted NOTOSANS reader/content IDs to canonical SD epdfont '%s'; "
                "kept UI_10/UI_12/SMALL on compact builtin subset "
                "(fixed generated pixel size ~%dpt)\n",
                familyName, M4FontPolicy::kCanonicalEpdfontPixelSize);
}

#ifdef CROSSPOINT_MURPHY_M4
void bindSystemReader(GfxRenderer& renderer, int targetPx) {
  const EpdFont* source = renderer.getFontPtr(NOTOSANS_16_FONT_ID);
  if (!source || source == &scaledSystemReader) source = compactSystemReader;
  if (!source) {
    Serial.println("[M4-FONT] DIAG: no system reader source to scale");
    return;
  }
  // The compact builtin face is generated at 16pt but rasterizes at only
  // ~14px (advanceY=20, ascender=15), while the canonical SD epdfont is a real
  // 16px raster. Divide by the bound source's actual pixels or every
  // compact-sourced size lands ~12% small relative to custom TTF.
  const bool compactSource = source == compactSystemReader;
  const float scale = static_cast<float>(targetPx) /
                      static_cast<float>(M4FontPolicy::systemReaderSourcePx(compactSource));
  scaledSystemReader.bind(source, scale);
  renderer.replaceFont(NOTOSANS_16_FONT_ID, EpdFontFamily(&scaledSystemReader));
  Serial.printf("[M4-FONT] System reader=%dpx scale=%.3f source=%s\n", targetPx, scaledSystemReader.scale(),
                compactSource ? "compact-2bit" : "canonical-epdfont");
}
#endif
}  // namespace

void EpdFontLoader::ensureFontsFromSd(GfxRenderer& renderer) {
  if (sdFontsLoaded_) {
    return;
  }
  Serial.printf("[M4-FONT] ensureFontsFromSd: first load this session\n");
  loadFontsFromSd(renderer);
}

bool EpdFontLoader::loadFontsFromSd(GfxRenderer& renderer) {
  const std::vector<int> previousCustomIds = loadedCustomIds;
  loadedCustomIds.clear();
  lastCanonicalResult = M4FontPolicy::LoadResult::NotAttempted;
#ifdef CROSSPOINT_MURPHY_M4
  captureCompactSystemReader(renderer);
#endif
  FontManager::getInstance().invalidateScan();
  const auto& families = FontManager::getInstance().getAvailableFamilies();

#ifdef CROSSPOINT_MURPHY_M4
  bool customLoadSucceeded = SETTINGS.fontFamily != CrossPointSettings::FONT_CUSTOM;
  M4FontPolicy::Inputs in;
  in.availableFamilies = families;
  in.hasCanonical = std::find(families.begin(), families.end(), M4FontPolicy::kCanonicalFamily) != families.end();
  in.mode = (SETTINGS.fontFamily == CrossPointSettings::FONT_CUSTOM) ? M4FontPolicy::FamilyMode::Custom
                                                                     : M4FontPolicy::FamilyMode::System;
  in.customFamily = SETTINGS.customFontFamily;

  const M4FontPolicy::Decision d = M4FontPolicy::decide(in);
  char decision[256];
  snprintf(decision, sizeof(decision), "decision setting=%s selected=%s load=%s reader_px=%d legacy_font_size=%d",
           SETTINGS.fontFamily == CrossPointSettings::FONT_CUSTOM ? "custom" : "system",
           SETTINGS.customFontFamily, d.loadCustomFamily.c_str(), SETTINGS.getReaderPixelSize(), SETTINGS.fontSize);
  FontManager::appendFontDiagnostic(decision);
  if (d.mutateSettingsToCustom) {
    SETTINGS.fontFamily = CrossPointSettings::FONT_CUSTOM;
    SETTINGS.saveToFile();
  }

  if (!d.diagnostic.empty()) {
    Serial.printf("[M4-FONT] DIAG: %s\n", d.diagnostic.c_str());
  }

  if (!in.hasCanonical) {
    lastCanonicalResult = M4FontPolicy::LoadResult::Missing;
  }

  const bool runtimeTtf = !d.loadCustomFamily.empty() && isRuntimeTtfFamily(d.loadCustomFamily);
  int runtimeReaderSize = -1;
  bool reuseRuntimeTtf = false;
  if (runtimeTtf) {
    runtimeReaderSize = SETTINGS.getReaderPixelSize();
    const int runtimeId = hashFontId(d.loadCustomFamily.c_str(), runtimeReaderSize);
    reuseRuntimeTtf = activeRuntimeTtfFamily == d.loadCustomFamily && activeRuntimeTtfSize == runtimeReaderSize &&
                      renderer.hasFont(runtimeId);
  }

  if (!reuseRuntimeTtf) {
    // Always restore builtin chrome before tearing down reader faces so a
    // failed/partial next font (or a stale custom-chrome mapping from older
    // builds) can never leave SMALL/UI_10/UI_12 dangling or custom-backed.
    M4FixedRuntimeUiFonts::restore(renderer);
#ifdef CROSSPOINT_MURPHY_M4
    const int oldReaderId = activeRuntimeTtfSize > 0
        ? hashFontId(activeRuntimeTtfFamily.c_str(), activeRuntimeTtfSize)
        : -1;
    logFontMap(renderer, "before_reader_release", oldReaderId);
#endif
    for (int id : previousCustomIds) renderer.removeFont(id);
    FontManager::getInstance().releaseRuntimeTtfFaces();
    FontManager::getInstance().clearLoadedFonts();
    activeRuntimeTtfFamily.clear();
    activeRuntimeTtfSize = -1;
    logFontHeap("after_release");
  } else {
    Serial.printf("[M4-FONT] Reusing runtime TTF face '%s' @%dpx\n",
                  d.loadCustomFamily.c_str(), runtimeReaderSize);
  }

  // 1) Explicit CUSTOM family -> reader hash IDs only. Runtime sfnt owns one
  // Reader rasterizer at the selected body size. System/plugin chrome stays on
  // the builtin faces permanently; custom Reader fonts must never replace
  // SMALL/UI_10/UI_12 (that leak made settings UI tiny/overlapped).
  if (!d.loadCustomFamily.empty()) {
    std::vector<int> sizes;
    if (runtimeTtf) {
      sizes.push_back(runtimeReaderSize);
    } else {
      // Preserve legacy epdfont behavior; its fixed bitmap artifact is cheap
      // compared with the runtime sfnt rasterizer and existing IDs depend on it.
      sizes = {12, 14, 16, 18, 20, 24};
      const uint8_t explicitSize = SETTINGS.getReaderPixelSize();
      if (std::find(sizes.begin(), sizes.end(), explicitSize) == sizes.end()) {
        sizes.push_back(explicitSize);
      }
    }

    bool any = false;
    for (int sz : sizes) {
      any = (loadAndInsertCustom(renderer, d.loadCustomFamily.c_str(), sz, loadedCustomIds) >= 0) || any;
    }
    if (!any) {
      customLoadSucceeded = false;
      Serial.printf("[M4-FONT] DIAG: failed to load explicit custom '%s'\n", d.loadCustomFamily.c_str());
      FontManager::appendFontDiagnostic("custom_result=failed");
      if (runtimeTtf) {
        activeRuntimeTtfFamily.clear();
        activeRuntimeTtfSize = -1;
      }
    } else if (runtimeTtf) {
      customLoadSucceeded = true;
      activeRuntimeTtfFamily = d.loadCustomFamily;
      activeRuntimeTtfSize = runtimeReaderSize;
      // ensure() is a restore-only no-op for chrome promotion.
      (void)M4FixedRuntimeUiFonts::ensure(renderer, d.loadCustomFamily.c_str());
      Serial.printf("[M4-FONT] Runtime sfnt '%s' reader=%dpx; UI chrome=builtin (no custom promotion)\n",
                    d.loadCustomFamily.c_str(), runtimeReaderSize);
      logFontMap(renderer, "runtime_reader_ready",
                 hashFontId(d.loadCustomFamily.c_str(), runtimeReaderSize));
      logFontHeap("runtime_ttf_ready");
    } else {
      customLoadSucceeded = true;
      FontManager::appendFontDiagnostic("custom_result=ok");
      Serial.printf("[M4-FONT] Loaded explicit CUSTOM family '%s' for reader\n", d.loadCustomFamily.c_str());
      (void)M4FixedRuntimeUiFonts::ensure(renderer, d.loadCustomFamily.c_str());
    }
  }

  if (runtimeTtf && reuseRuntimeTtf) {
    (void)M4FixedRuntimeUiFonts::ensure(renderer, d.loadCustomFamily.c_str());
    Serial.printf("[M4-FONT] Reused runtime sfnt; UI chrome remains builtin\n");
    logFontHeap("runtime_ttf_reused");
  }

  // 2) System/UI promotion: canonical only (never families.front() / Latin-only).
  if (!d.promoteSystemFamily.empty()) {
    EpdFontFamily* fam = FontManager::getInstance().getCustomFontFamily(
        d.promoteSystemFamily, M4FontPolicy::kCanonicalEpdfontPixelSize);
    if (!fam) {
      Serial.printf("[M4-FONT] DIAG: failed to load canonical '%s' for system promotion\n",
                    d.promoteSystemFamily.c_str());
      lastCanonicalResult = M4FontPolicy::LoadResult::LoadFailed;
    } else {
      promoteToReaderIds(renderer, d.promoteSystemFamily.c_str(), M4FontPolicy::kCanonicalEpdfontPixelSize);
      lastCanonicalResult = M4FontPolicy::LoadResult::Promoted;
    }
  } else if (in.mode == M4FontPolicy::FamilyMode::System) {
    if (lastCanonicalResult == M4FontPolicy::LoadResult::NotAttempted) {
      lastCanonicalResult = M4FontPolicy::LoadResult::Missing;
    }
    Serial.printf("[M4-FONT] DIAG: copy %s to SD for full reader/app CJK; compact UI subset otherwise. "
                  "Other epdfonts are never auto-promoted.\n",
                  M4FontPolicy::kCanonicalSdPath);
  }
#ifdef CROSSPOINT_MURPHY_M4
  // The selected custom face uses its hash ID above. NOTOSANS_16 remains the
  // canonical system fallback (and is also used for system mode); bind that
  // one stable ID to the exact requested pixel size in either case.
  bindSystemReader(renderer, SETTINGS.getReaderPixelSize());
  logFontMap(renderer, "load_complete", SETTINGS.getReaderFontId());
#endif
  sdFontsLoaded_ = true;
  return customLoadSucceeded;
#endif

  // Non-M4: original behavior — reload only the selected custom face.
  for (int id : previousCustomIds) renderer.removeFont(id);
  FontManager::getInstance().releaseRuntimeTtfFaces();
  FontManager::getInstance().clearLoadedFonts();
  bool legacyCustomLoadSucceeded = SETTINGS.fontFamily != CrossPointSettings::FONT_CUSTOM;
  if (SETTINGS.fontFamily == CrossPointSettings::FONT_CUSTOM) {
    if (strlen(SETTINGS.customFontFamily) > 0) {
      Serial.printf("Loading custom font: %s size %d\n", SETTINGS.customFontFamily,
                    SETTINGS.getReaderPixelSize());
      Serial.flush();
      const int size = SETTINGS.getReaderPixelSize();
      legacyCustomLoadSucceeded = loadAndInsertCustom(renderer, SETTINGS.customFontFamily, size, loadedCustomIds) >= 0;
    }
  }
  sdFontsLoaded_ = true;
  return legacyCustomLoadSucceeded;
}

int EpdFontLoader::getBestFontId(const char* familyName, int size) {
  if (!familyName || strlen(familyName) == 0) return -1;

  const int id = hashFontId(familyName, size);
  for (int loadedId : loadedCustomIds) {
    if (loadedId == id) return id;
  }
#ifdef CROSSPOINT_MURPHY_M4
  // The active runtime face is the source of truth. Bookkeeping is rebuilt on
  // every SD/font refresh, but a successfully retained sfnt face and renderer
  // mapping must never silently degrade to the compact OMIT_FONTS fallback.
  if (activeRuntimeTtfFamily == familyName) {
    if (activeRuntimeTtfSize == size) return id;
    if (activeRuntimeTtfSize > 0) return hashFontId(activeRuntimeTtfFamily.c_str(), activeRuntimeTtfSize);
  }
#endif
  return -1;
}
