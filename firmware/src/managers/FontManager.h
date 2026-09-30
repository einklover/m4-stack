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

  // Which UI role a runtime TTF face serves. The PSRAM budget follows the
  // role — never creation order (B6):
  // - Reader: the main reading face, 512 slots / 768KB.
  // - Chrome: system UI small/body faces, 96 slots / 96KB.
  enum class TtfFaceRole : uint8_t { Reader, Chrome };

  // Load a specific family and size (returns pointer to cached family or new one)
  EpdFontFamily* getCustomFontFamily(const std::string& familyName, int fontSize, TtfFaceRole role);

  // 清除已加载字体的内存缓存（切换字体时调用，迫使重新加载并写入 flash）
  void clearLoadedFonts();
  void clearLoadedReaderFonts();

  // Runtime TTF objects own their stream/cmap/scratch/PSRAM cache metadata.
  // Once GfxRenderer aliases have been removed, they can and should be fully
  // destroyed on a real family/reader-size switch. The legacy clear path only
  // clears caches because historical epdfont objects have mixed ownership;
  // keeping this operation TTF-only avoids changing that legacy contract.
  void releaseRuntimeTtfFaces();
  void releaseRuntimeTtfFaces(TtfFaceRole role);
  // Delete detached runtime faces for one role while preserving the currently
  // bound family/sizes. Used by system chrome after aliases have moved to the
  // new face, preventing old UI-size variants from accumulating in PSRAM.
  void releaseRuntimeTtfFacesExcept(TtfFaceRole role, const std::string& familyName,
                                    int keepSizeA, int keepSizeB = -1);

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

  // Runtime TTF: (family, sizePx, role) so chrome/reader at the same px do not
  // share a budget. epdfont ignores role (always 0).
  struct LoadedFaceKey {
    int sizePx = 0;
    uint8_t role = 0;
    bool operator<(const LoadedFaceKey& o) const {
      if (sizePx != o.sizePx) return sizePx < o.sizePx;
      return role < o.role;
    }
  };
  std::map<std::string, std::map<LoadedFaceKey, EpdFontFamily*>> loadedFonts;
};
