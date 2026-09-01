#pragma once

#include <cstdint>

// Target-sized, fixed-point cover processing.  The caller owns the buffers;
// this keeps the algorithm usable by JPEG, PNG, and BMP conversion paths.
namespace M4CoverDither {

constexpr int kBlackCut = 56;
constexpr int kWhiteCut = 202;
constexpr int kEdgeThreshold = 32;
constexpr int kEdgeRetention = 62;
constexpr int kEdgeBoost = 24;
constexpr int kEdgeGateScale = 4;
constexpr int kDetailClip = 28;
constexpr int kLocalPush = 32;
constexpr int kEdgeHardness = 72;

static const uint8_t kRangeWeights[64] = {
    255, 251, 241, 225, 204, 180, 155, 129, 105, 83, 64, 47, 35, 24, 17, 11,
    7, 5, 3, 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

// 8x8 ordered blue-noise-like screen. Every mark is one pixel; the Bayer
// ordering keeps each successive tone level spatially dispersed and cheap.
static const uint8_t kDotRanks[64] = {
     0, 32,  8, 40,  2, 34, 10, 42,
    48, 16, 56, 24, 50, 18, 58, 26,
    12, 44,  4, 36, 14, 46,  6, 38,
    60, 28, 52, 20, 62, 30, 54, 22,
     3, 35, 11, 43,  1, 33,  9, 41,
    51, 19, 59, 27, 49, 17, 57, 25,
    15, 47,  7, 39, 13, 45,  5, 37,
    63, 31, 55, 23, 61, 29, 53, 21,
};

inline uint8_t clampByte(int value) {
  if (value < 0) return 0;
  if (value > 255) return 255;
  return static_cast<uint8_t>(value);
}

inline uint8_t sample(const uint8_t* image, int width, int height, int x, int y) {
  if (x < 0) x = 0;
  if (x >= width) x = width - 1;
  if (y < 0) y = 0;
  if (y >= height) y = height - 1;
  return image[y * width + x];
}

inline void bilateralPass(const uint8_t* source, uint8_t* destination,
                          int width, int height) {
  static const uint8_t spatial[3][3] = {{1, 2, 1}, {2, 4, 2}, {1, 2, 1}};
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const int center = source[y * width + x];
      uint32_t weighted = 0;
      uint32_t weights = 0;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          const int value = sample(source, width, height, x + dx, y + dy);
          const int delta = value > center ? value - center : center - value;
          const uint32_t weight = spatial[dy + 1][dx + 1] * kRangeWeights[delta >> 2];
          weighted += static_cast<uint32_t>(value) * weight;
          weights += weight;
        }
      }
      destination[y * width + x] = static_cast<uint8_t>((weighted + weights / 2) / weights);
    }
  }
}

inline int percentile(const uint32_t* histogram, int total, int percent) {
  const uint32_t target = static_cast<uint32_t>((total - 1) * percent / 100);
  uint32_t seen = 0;
  for (int value = 0; value < 256; ++value) {
    seen += histogram[value];
    if (seen > target) return value;
  }
  return 255;
}

inline void applyToneCurve(uint8_t* image, int width, int height) {
  uint32_t histogram[256] = {};
  const int total = width * height;
  for (int i = 0; i < total; ++i) ++histogram[image[i]];
  const int blackPoint = percentile(histogram, total, 2);
  const int whitePoint = blackPoint < percentile(histogram, total, 98)
                             ? percentile(histogram, total, 98)
                             : blackPoint + 1;
  const int span = whitePoint - blackPoint;
  uint8_t lut[256];
  for (int value = 0; value < 256; ++value) {
    const int normalized = ((value - blackPoint) * 255) / span;
    lut[value] = clampByte(((normalized - 128) * 108) / 100 + 128);
  }
  for (int i = 0; i < total; ++i) image[i] = lut[image[i]];
}

inline int coherentEdge(const uint8_t* image, int width, int height, int x, int y) {
  const int horizontal = static_cast<int>(sample(image, width, height, x - 1, y)) -
                         static_cast<int>(sample(image, width, height, x + 1, y));
  const int vertical = static_cast<int>(sample(image, width, height, x, y - 1)) -
                       static_cast<int>(sample(image, width, height, x, y + 1));
  const int diagonalA = static_cast<int>(sample(image, width, height, x - 1, y - 1)) -
                        static_cast<int>(sample(image, width, height, x + 1, y + 1));
  const int diagonalB = static_cast<int>(sample(image, width, height, x + 1, y - 1)) -
                        static_cast<int>(sample(image, width, height, x - 1, y + 1));
  int result = horizontal < 0 ? -horizontal : horizontal;
  const int values[] = {vertical, diagonalA, diagonalB};
  for (int value : values) {
    if (value < 0) value = -value;
    if (value > result) result = value;
  }
  return result;
}

// Prepare one target-sized grayscale plane. work becomes the edge map and
// smooth is the second bilateral pass. No floating point or error rows.
inline bool prepare(uint8_t* image, uint8_t* work, uint8_t* smooth,
                    int width, int height) {
  if (!image || !work || !smooth || width <= 0 || height <= 0) return false;
  applyToneCurve(image, width, height);
  bilateralPass(image, work, width, height);
  bilateralPass(work, smooth, width, height);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      work[y * width + x] = static_cast<uint8_t>(coherentEdge(smooth, width, height, x, y));
    }
  }
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const int index = y * width + x;
      const int edge = work[index];
      int gate = (edge - kEdgeThreshold) * kEdgeGateScale;
      if (gate < 0) gate = 0;
      if (gate > 255) gate = 255;
      const int retained = gate * kEdgeRetention / 100;
      const int raw = image[index];
      const int soft = smooth[index];
      int tone = (soft * (255 - retained) + raw * retained + 127) / 255;
      int detail = raw - soft;
      if (detail < -kDetailClip) detail = -kDetailClip;
      if (detail > kDetailClip) detail = kDetailClip;
      tone += detail * gate * kEdgeBoost / (255 * 100);
      int localSum = 0;
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
          localSum += sample(smooth, width, height, x + dx, y + dy);
      tone += (soft - localSum / 9) * gate * kLocalPush / (255 * 100);
      image[index] = clampByte(tone);
    }
  }
  return true;
}

inline uint8_t pixelToBit(const uint8_t* tone, const uint8_t* edge,
                          int width, int x, int y) {
  const int value = tone[y * width + x];
  if (value <= kBlackCut) return 0;
  if (value >= kWhiteCut) return 1;
  const int span = kWhiteCut - kBlackCut;
  const int toneLevel = (value - kBlackCut) * 255 / span;
  const int rank = kDotRanks[(y & 7) * 8 + (x & 7)];
  int threshold = (rank * 255 + 128) / 64;
  int gate = (static_cast<int>(edge[y * width + x]) - kEdgeThreshold) * kEdgeGateScale;
  if (gate < 0) gate = 0;
  if (gate > 255) gate = 255;
  const int hardness = gate * kEdgeHardness / 100;
  threshold += (128 - threshold) * hardness / 255;
  return toneLevel >= threshold ? 1 : 0;
}

}  // namespace M4CoverDither
