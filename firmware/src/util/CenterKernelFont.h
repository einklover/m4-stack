#pragma once

// Host-testable center-kernel math for reader body and system UI CJK.
//
// Canonical stored glyph: 16x16 absolute center occupancy + 2-bit joint class
// from the TTF (pixel_center_absolute_report.json). Rendering is 1-bit
// integer-pixel Kx×Ky rectangles OR-composited from those centers.
// rhu(x)=floor(x+0.5). Never banker's round / std::round / lround.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace CenterKernelFont {

constexpr int kCellPx = 16;
constexpr uint8_t kDefaultPx = 26;

struct JointClass {
  uint16_t advanceUpm;
  float phaseDelta;
  uint8_t kxUpm;
  uint8_t kyUpm;
};

// Joint ids match pixel_center_absolute_report.json joint_metric_classes.
constexpr JointClass kClasses[4] = {
    {960, -49.5f, 75, 75},  // 0: ~18376, 鬱
    {1000, 0.0f, 74, 75},   // 1: ~9172, 一十中田自我你河明国
    {1000, 0.5f, 75, 75},   // 2: 4, 寝寫胀脑
    {1000, 30.0f, 74, 75},  // 3: 1, 猫
};

inline int rhu(double x) {
  return static_cast<int>(std::floor(x + 0.5));
}

inline double pitchP(int n) { return static_cast<double>(n) / static_cast<double>(kCellPx); }

inline int kernelX(int n, int cls) {
  if (cls < 0 || cls > 3) cls = 1;
  const int k = rhu(pitchP(n) * static_cast<double>(kClasses[cls].kxUpm) / 60.0);
  return std::max(1, k);
}

inline int kernelY(int n, int cls) {
  if (cls < 0 || cls > 3) cls = 1;
  const int k = rhu(pitchP(n) * static_cast<double>(kClasses[cls].kyUpm) / 60.0);
  return std::max(1, k);
}

inline void kernelSize(int n, int cls, int* kx, int* ky) {
  if (kx) *kx = kernelX(n, cls);
  if (ky) *ky = kernelY(n, cls);
}

inline int centerX(int n, int cls, int col) {
  if (cls < 0 || cls > 3) cls = 1;
  const double p = pitchP(n);
  const double x = (80.0 + static_cast<double>(kClasses[cls].phaseDelta) + 60.0 * col) / 60.0 * p;
  return rhu(x);
}

inline int centerY(int n, int row) { return rhu(static_cast<double>(row) * pitchP(n)); }

inline int origin0(int center, int k) { return center - (k - 1) / 2; }

inline int advancePx(int n, int cls) {
  if (cls < 0 || cls > 3) cls = 1;
  return rhu(static_cast<double>(n) * static_cast<double>(kClasses[cls].advanceUpm) / 960.0);
}

inline int lineHeight(int n) { return n; }

}  // namespace CenterKernelFont
