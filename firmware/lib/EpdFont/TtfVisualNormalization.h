#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Runtime outline fonts use different em proportions.  The compact CJK
// fallback is the stable visual contract: its `口` ink box is 12px in the
// canonical 16px artifact.  Keep this policy independent of Arduino so the
// host font matrix can exercise the exact same calculation.
namespace M4TtfVisualNormalization {

constexpr uint16_t kCanonicalReferencePx = 16;
constexpr uint16_t kCanonicalReferenceHeightPx = 12;
constexpr float kMinVisualScale = 0.5f;
constexpr float kMaxVisualScale = 2.0f;

static constexpr uint32_t kReferenceCodepoints[] = {
    0x53E3,  // 口
    0x56FD,  // 国
    0x7530,  // 田
    0x4E2D,  // 中
    0x6C38,  // 永
    0x76EE,  // 目
    'H',
    'M',
};

constexpr size_t kReferenceCodepointCount =
    sizeof(kReferenceCodepoints) / sizeof(kReferenceCodepoints[0]);

inline float targetReferenceHeight(uint16_t nominalPx) {
  return static_cast<float>(nominalPx) *
         static_cast<float>(kCanonicalReferenceHeightPx) /
         static_cast<float>(kCanonicalReferencePx);
}

inline float scaleForReference(uint16_t nominalPx, float referenceHeightPx) {
  if (nominalPx == 0 || referenceHeightPx <= 0) return 1.0f;
  const float raw = targetReferenceHeight(nominalPx) /
                    static_cast<float>(referenceHeightPx);
  return std::max(kMinVisualScale, std::min(kMaxVisualScale, raw));
}

inline uint16_t renderPixelSize(uint16_t nominalPx, float visualScale) {
  const int px = static_cast<int>(std::lround(
      static_cast<float>(std::max<uint16_t>(1, nominalPx)) * visualScale));
  return static_cast<uint16_t>(std::max(1, std::min(255, px)));
}

}  // namespace M4TtfVisualNormalization
