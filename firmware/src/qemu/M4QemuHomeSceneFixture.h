#pragma once

#ifndef M4_QEMU_BUILD
#error "M4QemuHomeSceneFixture is QEMU-only"
#endif

#include <cstddef>
#include <cstdint>

#include "ui/pages/HomeSceneModel.h"
#include "ui/scene/UiSceneAssets.h"

namespace M4QemuHomeSceneFixture {

namespace Detail {

template <uint16_t Width, uint16_t Height>
struct Bitmap {
  static constexpr uint16_t kStride = (Width + 7) / 8;
  uint8_t pixels[static_cast<std::size_t>(kStride) * Height]{};

  void set(uint16_t x, uint16_t y) {
    if (x < Width && y < Height) {
      pixels[static_cast<std::size_t>(y) * kStride + (x >> 3)] |=
          static_cast<uint8_t>(0x80u >> (x & 7u));
    }
  }

  UiScene::UiSceneAsset asset() const {
    return {pixels, Width, Height, kStride, false};
  }
};

template <uint16_t Width, uint16_t Height>
void line(Bitmap<Width, Height>& bitmap, int x0, int y0, int x1, int y1) {
  const int dx = x1 > x0 ? x1 - x0 : x0 - x1;
  const int sx = x0 < x1 ? 1 : -1;
  const int dy = -(y1 > y0 ? y1 - y0 : y0 - y1);
  const int sy = y0 < y1 ? 1 : -1;
  int error = dx + dy;
  for (;;) {
    bitmap.set(static_cast<uint16_t>(x0), static_cast<uint16_t>(y0));
    if (x0 == x1 && y0 == y1) break;
    const int twice = 2 * error;
    if (twice >= dy) {
      error += dy;
      x0 += sx;
    }
    if (twice <= dx) {
      error += dx;
      y0 += sy;
    }
  }
}

template <uint16_t Width, uint16_t Height>
void cover(Bitmap<Width, Height>& bitmap, uint8_t motif) {
  line(bitmap, 1, 1, Width - 2, 1);
  line(bitmap, Width - 2, 1, Width - 2, Height - 2);
  line(bitmap, Width - 2, Height - 2, 1, Height - 2);
  line(bitmap, 1, Height - 2, 1, 1);
  for (uint16_t y = 16 + motif * 3; y < Height / 2; y += 13) {
    line(bitmap, 10, y, Width - 11, y);
  }
  line(bitmap, 10, Height - 18, Width - 11, Height / 2 + motif * 2);
  line(bitmap, Width / 3, Height - 18, Width - 11, Height - 18);
}

struct Storage {
  Bitmap<110, 180> current;
  Bitmap<74, 106> recent[3];
  Bitmap<62, 64> icons[4];

  Storage() {
    cover(current, 0);
    for (uint8_t i = 0; i < 3; ++i) cover(recent[i], i + 1);

    // History: clock; Books: open book; Settings: sliders; Plugins: plug.
    for (int x = 14; x <= 54; ++x) {
      icons[0].set(x, 14); icons[0].set(x, 54);
    }
    for (int y = 14; y <= 54; ++y) {
      icons[0].set(14, y); icons[0].set(54, y);
    }
    line(icons[0], 34, 34, 34, 21); line(icons[0], 34, 34, 45, 40);

    line(icons[1], 8, 15, 31, 20); line(icons[1], 31, 20, 31, 55);
    line(icons[1], 31, 20, 59, 15); line(icons[1], 59, 15, 59, 51);
    line(icons[1], 8, 15, 8, 51); line(icons[1], 8, 51, 31, 57);
    line(icons[1], 31, 57, 59, 51);

    for (int y = 17; y <= 51; y += 17) line(icons[2], 10, y, 58, y);
    for (int x = 20; x <= 48; x += 14) {
      line(icons[2], x, 12, x, 22); line(icons[2], x, 29, x, 39);
      line(icons[2], x, 46, x, 56);
    }

    line(icons[3], 25, 9, 25, 24); line(icons[3], 43, 9, 43, 24);
    line(icons[3], 18, 24, 50, 24); line(icons[3], 18, 24, 18, 39);
    line(icons[3], 50, 24, 50, 39); line(icons[3], 18, 39, 50, 39);
    line(icons[3], 34, 39, 34, 58); line(icons[3], 27, 58, 41, 58);
  }
};

inline Storage& storage() {
  static Storage value;
  return value;
}

}  // namespace Detail

inline bool populate(UiScene::UiSceneAssets& assets) {
  Detail::Storage& data = Detail::storage();
  assets.clear();
  bool ok = assets.add({HomeScene::kBindingCurrentCover,
                        UiScene::kInvalidBindingId,
                        UiScene::kInvalidAssetItemIndex},
                       data.current.asset());
  for (uint8_t i = 0; i < 3; ++i) {
    ok = assets.add({HomeScene::kBindingItemCover, HomeScene::kBindingRecent, i},
                    data.recent[i].asset()) && ok;
  }
  for (uint8_t i = 0; i < 4; ++i) {
    ok = assets.add({HomeScene::kBindingItemIcon, HomeScene::kBindingApps, i},
                    data.icons[i].asset()) && ok;
  }
  return ok && assets.count == UiScene::UiSceneAssets::kMaxAssets;
}

}  // namespace M4QemuHomeSceneFixture
