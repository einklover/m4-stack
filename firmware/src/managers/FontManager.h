#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "EpdFontFamily.h"

class FontManager {
 public:
  struct RuntimeFontInfo {
    std::string filename;
    std::string displayName;
    std::string type;
    uint32_t sizeBytes = 0;
    // Bounded on-device metadata only; no font payload leaves the device.
    char signature[8] = {};
    char integrity[32] = {};
  };

  struct RuntimeFontDiagnostic {
    bool attempted = false;
    bool ok = false;
    char filename[128] = {};
    char stage[24] = {};
    char error[160] = {};
  };

  static FontManager& getInstance();

  // Scan SD card for fonts
  void scanFonts();

  // Get list of available font family names
  const std::vector<std::string>& getAvailableFamilies();

  // User-facing runtime font list. Legacy .epdfont files remain available to
  // the internal loader, but are intentionally not exposed by the picker.
  const std::vector<std::string>& getAvailableTtfFamilies();
  const std::vector<RuntimeFontInfo>& getRuntimeFonts();
  static RuntimeFontDiagnostic lastRuntimeFontDiagnostic();

  // Persist font diagnostics because serial output is not reliable on USB.
  static void appendFontDiagnostic(const char* line);

  // Load a specific family and size (returns pointer to cached family or new one)
  EpdFontFamily* getCustomFontFamily(const std::string& familyName, int fontSize);

  // 清除已加载字体的内存缓存（切换字体时调用，迫使重新加载并写入 flash）
  void clearLoadedFonts();

  // Runtime TTF objects own their stream/cmap/scratch/PSRAM cache metadata.
  // Once GfxRenderer aliases have been removed, they can and should be fully
  // destroyed on a real family/reader-size switch. The legacy clear path only
  // clears caches because historical epdfont objects have mixed ownership;
  // keeping this operation TTF-only avoids changing that legacy contract.
  void releaseRuntimeTtfFaces() {
    for (auto familyIt = loadedFonts.begin(); familyIt != loadedFonts.end();) {
      auto& sizes = familyIt->second;
      for (auto sizeIt = sizes.begin(); sizeIt != sizes.end();) {
        EpdFontFamily* family = sizeIt->second;
        const EpdFont* font = family ? family->getFont(EpdFontFamily::REGULAR) : nullptr;
        if (font && font->isRuntimeTtf()) {
          delete const_cast<EpdFont*>(font);
          delete family;
          sizeIt = sizes.erase(sizeIt);
        } else {
          ++sizeIt;
        }
      }
      if (sizes.empty()) {
        familyIt = loadedFonts.erase(familyIt);
      } else {
        ++familyIt;
      }
    }
  }

  // Force next getAvailableFamilies() to re-scan /fonts and /FONT (M4 hot-plug / first boot).
  void invalidateScan() {
    scanned = false;
    availableFamilies.clear();
    availableTtfFamilies.clear();
    runtimeFonts.clear();
  }

 private:
  FontManager() = default;
  ~FontManager();

  std::vector<std::string> availableFamilies;
  std::vector<std::string> availableTtfFamilies;
  std::vector<RuntimeFontInfo> runtimeFonts;
  bool scanned = false;

  // Map: FamilyName -> Size -> EpdFontFamily*
  std::map<std::string, std::map<int, EpdFontFamily*>> loadedFonts;
};
