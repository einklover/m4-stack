#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "ui/pages/HomeSceneModel.h"

namespace HomeSceneAssetDecoder {

// Host-testable pure BMP validation + 1-bit decode from memory.
// On device the file path variant uses SdMan + Bitmap; this pure helper is used for tests and as inner decode.
bool decodeBmpBytesTo1Bit(const uint8_t* bmpData, size_t bmpLen, uint8_t* out1Bit,
                          uint16_t expW, uint16_t expH, uint16_t expStride,
                          std::function<bool()> isCancelled = nullptr);

// File decode with cancellation between rows and strict validation.
// Returns true on full valid decode into out1Bit (size stride*height). On any
// error/missing/malformed/short/OOM/cancel returns false and leaves out unchanged (degrades to missing asset).
// Must not call GfxRenderer, must not allocate on render path (backend only).
bool decodeBmpFileTo1Bit(const char* path, uint8_t* out1Bit, uint16_t expW, uint16_t expH,
                         uint16_t expStride, std::function<bool()> isCancelled = nullptr);

// Resolve an installed app icon file path safely from registry metadata.
// Returns empty if not safe/empty/unsafe (prevents directory traversal).
std::string resolveAppIconPath(const std::string& installPath, const std::string& iconField);

// Validate thumb path generation — thin wrapper so tests can prove no FS call on render.
inline std::string thumbPathForCover(const std::string& coverBmpPath, int w, int h) {
  // Reuse UITheme::getCoverThumbPath on device; pure replace for host testability without UITheme dependency.
  std::string p = coverBmpPath;
  const std::string widthTag = "[WIDTH]";
  const std::string heightTag = "[HEIGHT]";
  size_t pos = p.find(widthTag);
  if (pos != std::string::npos) p.replace(pos, widthTag.size(), std::to_string(w));
  pos = p.find(heightTag);
  if (pos != std::string::npos) p.replace(pos, heightTag.size(), std::to_string(h));
  return p;
}

// Backend helpers — each checks isCancelled between items and inside decode.
bool decodeCoverForPublication(HomeScene::HomeScenePublication& pub,
                               const char* coverPathOrBmpPath,
                               const UiScene::AssetKey& key,
                               std::function<bool()> isCancelled = nullptr);

bool decodeAppIconForPublication(HomeScene::HomeScenePublication& pub,
                                 const std::string& installPath,
                                 const std::string& iconField,
                                 const UiScene::AssetKey& key,
                                 std::function<bool()> isCancelled = nullptr);

// Builtin files icon (no SD path). Decodes the compiled 1-bit 62x64 icon
// for builtin.files slot. Works on device and host without filesystem.
bool decodeBuiltinFilesIconForPublication(HomeScene::HomeScenePublication& pub,
                                          const UiScene::AssetKey& key,
                                          std::function<bool()> isCancelled = nullptr);

// Return a sheet-derived 62x64 1-bit icon for a matched builtin app id.
// Bytes are row-major, MSB-first, with 1 meaning black ink; nullptr means no sheet match.
const uint8_t* builtinSheetIcon(const char* id);

// Host helper — fills a small compiled 1-bit fallback icon (used when BMP missing but we want a placeholder asset).
bool fillFallbackAppIcon(uint8_t* out, uint16_t w, uint16_t h, uint16_t stride, uint8_t pattern);

}  // namespace HomeSceneAssetDecoder
