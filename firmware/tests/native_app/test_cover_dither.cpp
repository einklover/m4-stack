#include <cassert>
#include <cstdint>
#include <vector>

#include "CoverDither.h"

static int whitePixels(const std::vector<uint8_t>& tone, const std::vector<uint8_t>& edge,
                       int width, int height) {
  int count = 0;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      count += M4CoverDither::pixelToBit(tone.data(), edge.data(), width, x, y);
    }
  }
  return count;
}

int main() {
  constexpr int width = 64;
  constexpr int height = 64;
  std::vector<uint8_t> tone(width * height);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) tone[y * width + x] = static_cast<uint8_t>(80 + (x * 96) / (width - 1));
  }
  std::vector<uint8_t> work(width * height);
  std::vector<uint8_t> smooth(width * height);
  assert(M4CoverDither::prepare(tone.data(), work.data(), smooth.data(), width, height));

  const int flatWhite = whitePixels(tone, work, width, height);
  assert(flatWhite > width * height * 35 / 100);
  assert(flatWhite < width * height * 65 / 100);

  std::vector<uint8_t> edgeTone(width * height, 96);
  for (int y = 0; y < height; ++y) {
    for (int x = width / 2; x < width; ++x) edgeTone[y * width + x] = 220;
  }
  std::fill(work.begin(), work.end(), 0);
  std::fill(smooth.begin(), smooth.end(), 0);
  assert(M4CoverDither::prepare(edgeTone.data(), work.data(), smooth.data(), width, height));
  const int leftWhite = whitePixels(edgeTone, work, width, height / 2);
  const int rightWhite = whitePixels(edgeTone, work, width, height);
  assert(rightWhite > leftWhite + width * height / 5);

  return 0;
}
