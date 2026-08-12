#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "TtfReader.h"

namespace {

void be16(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back(static_cast<uint8_t>(x >> 8));
  v.push_back(static_cast<uint8_t>(x));
}
void beS16(std::vector<uint8_t>& v, int16_t x) { be16(v, static_cast<uint16_t>(x)); }
void be32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(static_cast<uint8_t>(x >> 24));
  v.push_back(static_cast<uint8_t>(x >> 16));
  v.push_back(static_cast<uint8_t>(x >> 8));
  v.push_back(static_cast<uint8_t>(x));
}
uint16_t get16(const std::vector<uint8_t>& v, size_t off) {
  return static_cast<uint16_t>((uint16_t(v[off]) << 8) | v[off + 1]);
}
uint32_t get32(const std::vector<uint8_t>& v, size_t off) {
  return (uint32_t(v[off]) << 24) | (uint32_t(v[off + 1]) << 16) |
         (uint32_t(v[off + 2]) << 8) | uint32_t(v[off + 3]);
}
void put16(std::vector<uint8_t>& v, size_t off, uint16_t x) {
  v[off] = static_cast<uint8_t>(x >> 8);
  v[off + 1] = static_cast<uint8_t>(x);
}
void putS16(std::vector<uint8_t>& v, size_t off, int16_t x) { put16(v, off, static_cast<uint16_t>(x)); }
void put32(std::vector<uint8_t>& v, size_t off, uint32_t x) {
  v[off] = static_cast<uint8_t>(x >> 24);
  v[off + 1] = static_cast<uint8_t>(x >> 16);
  v[off + 2] = static_cast<uint8_t>(x >> 8);
  v[off + 3] = static_cast<uint8_t>(x);
}
void pad4(std::vector<uint8_t>& v) {
  while (v.size() & 3u) v.push_back(0);
}

std::vector<uint8_t> simpleSquare(int16_t size) {
  std::vector<uint8_t> g;
  beS16(g, 1);
  beS16(g, 0); beS16(g, 0);
  beS16(g, size); beS16(g, size);
  be16(g, 3);
  be16(g, 0);
  for (int i = 0; i < 4; ++i) g.push_back(0x01);
  const int16_t xs[4] = {0, size, 0, static_cast<int16_t>(-size)};
  const int16_t ys[4] = {0, 0, size, 0};
  for (int16_t x : xs) beS16(g, x);
  for (int16_t y : ys) beS16(g, y);
  return g;
}

std::vector<uint8_t> compoundPointAttached() {
  std::vector<uint8_t> g;
  beS16(g, -1);
  beS16(g, 0); beS16(g, 0); beS16(g, 600); beS16(g, 600);

  be16(g, 0x0001 | 0x0002 | 0x0020);
  be16(g, 0);
  beS16(g, 0); beS16(g, 0);

  be16(g, 0x0001);
  be16(g, 1);
  be16(g, 2);
  be16(g, 0);
  return g;
}

struct TableBlob {
  std::string tag;
  std::vector<uint8_t> data;
};

std::vector<uint8_t> buildFont() {
  std::vector<TableBlob> tables;

  std::vector<uint8_t> head(54, 0);
  put32(head, 0, 0x00010000u);
  put32(head, 12, 0x5f0f3cf5u);
  put16(head, 18, 1000);
  putS16(head, 36, 0); putS16(head, 38, 0);
  putS16(head, 40, 600); putS16(head, 42, 600);
  putS16(head, 50, 1);
  tables.push_back({"head", head});

  std::vector<uint8_t> maxp(6, 0);
  put32(maxp, 0, 0x00010000u);
  put16(maxp, 4, 3);
  tables.push_back({"maxp", maxp});

  std::vector<uint8_t> hhea(36, 0);
  put32(hhea, 0, 0x00010000u);
  putS16(hhea, 4, 800); putS16(hhea, 6, -200);
  put16(hhea, 34, 3);
  tables.push_back({"hhea", hhea});

  std::vector<uint8_t> hmtx;
  for (int i = 0; i < 3; ++i) { be16(hmtx, 700); beS16(hmtx, 0); }
  tables.push_back({"hmtx", hmtx});

  std::vector<uint8_t> cmap;
  be16(cmap, 0); be16(cmap, 1);
  be16(cmap, 3); be16(cmap, 1); be32(cmap, 12);
  std::vector<uint8_t> fmt4;
  be16(fmt4, 4); be16(fmt4, 32); be16(fmt4, 0);
  be16(fmt4, 4); be16(fmt4, 4); be16(fmt4, 1); be16(fmt4, 0);
  be16(fmt4, 0x0041); be16(fmt4, 0xffff);
  be16(fmt4, 0);
  be16(fmt4, 0x0041); be16(fmt4, 0xffff);
  beS16(fmt4, static_cast<int16_t>(2 - 0x0041)); beS16(fmt4, 1);
  be16(fmt4, 0); be16(fmt4, 0);
  cmap.insert(cmap.end(), fmt4.begin(), fmt4.end());
  be16(cmap, 0);
  tables.push_back({"cmap", cmap});

  std::vector<uint8_t> glyf;
  std::vector<uint32_t> loca;
  loca.push_back(0);
  for (auto g : {simpleSquare(500), simpleSquare(100), compoundPointAttached()}) {
    glyf.insert(glyf.end(), g.begin(), g.end());
    if (glyf.size() & 1u) glyf.push_back(0);
    loca.push_back(static_cast<uint32_t>(glyf.size()));
  }
  std::vector<uint8_t> locaBlob;
  for (uint32_t off : loca) be32(locaBlob, off);
  tables.push_back({"loca", locaBlob});
  tables.push_back({"glyf", glyf});

  const uint16_t n = static_cast<uint16_t>(tables.size());
  std::vector<uint8_t> font(12u + static_cast<size_t>(n) * 16u, 0);
  put32(font, 0, 0x00010000u);
  put16(font, 4, n);

  uint32_t off = static_cast<uint32_t>(font.size());
  while (off & 3u) { font.push_back(0); ++off; }
  for (uint16_t i = 0; i < n; ++i) {
    const auto& t = tables[i];
    const size_t rec = 12u + static_cast<size_t>(i) * 16u;
    std::memcpy(font.data() + rec, t.tag.data(), 4);
    put32(font, rec + 4, 0);
    put32(font, rec + 8, off);
    put32(font, rec + 12, static_cast<uint32_t>(t.data.size()));
    font.insert(font.end(), t.data.begin(), t.data.end());
    pad4(font);
    off = static_cast<uint32_t>(font.size());
  }
  return font;
}

std::vector<uint8_t> wrapCollection(std::vector<uint8_t> face, uint32_t faceOff, bool otto) {
  assert(faceOff >= 16 && (faceOff & 3u) == 0);
  assert(face.size() >= 12);
  const uint16_t tables = get16(face, 4);
  assert(12u + uint32_t(tables) * 16u <= face.size());

  if (otto) put32(face, 0, 0x4f54544fu);
  for (uint16_t i = 0; i < tables; ++i) {
    const size_t rec = 12u + static_cast<size_t>(i) * 16u;
    put32(face, rec + 8, get32(face, rec + 8) + faceOff);
  }

  std::vector<uint8_t> collection(faceOff, 0);
  std::memcpy(collection.data(), "ttcf", 4);
  put32(collection, 4, 0x00010000u);
  put32(collection, 8, 1);
  put32(collection, 12, faceOff);
  collection.insert(collection.end(), face.begin(), face.end());
  return collection;
}

class VectorStream final : public ttf::TtfStream {
 public:
  explicit VectorStream(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}
  uint32_t size() const override { return static_cast<uint32_t>(bytes_.size()); }
  bool seek(uint32_t pos) override {
    if (pos > bytes_.size()) return false;
    pos_ = pos;
    return true;
  }
  uint32_t read(void* dst, uint32_t n) override {
    const uint32_t left = static_cast<uint32_t>(bytes_.size() - pos_);
    const uint32_t take = std::min(left, n);
    if (take) std::memcpy(dst, bytes_.data() + pos_, take);
    pos_ += take;
    return take;
  }
 private:
  std::vector<uint8_t> bytes_;
  uint32_t pos_ = 0;
};

void verifyCompound(ttf::TtfFont& font) {
  uint16_t gid = 0;
  assert(font.findGlyph('A', gid));
  assert(gid == 2);

  std::vector<ttf::Contour> contours;
  assert(font.collectGlyph(gid, ttf::Xform{}, contours));
  assert(contours.size() == 2);
  assert(contours[0].pts.size() == 4);
  assert(contours[1].pts.size() == 4);

  const auto& parentAnchor = contours[0].pts[2];
  const auto& childAnchor = contours[1].pts[0];
  assert(std::fabs(parentAnchor.x - childAnchor.x) < 0.001f);
  assert(std::fabs(parentAnchor.y - childAnchor.y) < 0.001f);
  assert(std::fabs(childAnchor.x - 500.0f) < 0.001f);
  assert(std::fabs(childAnchor.y - 500.0f) < 0.001f);

  ttf::GlyphBitmap bitmap;
  assert(font.rasterize(gid, 32, bitmap));
  assert(bitmap.width > 0 && bitmap.height > 0);
  assert(bitmap.data != nullptr && bitmap.packedLen > 0);
}

}  // namespace

int main() {
  {
    VectorStream stream(buildFont());
    ttf::TtfFont font;
    if (!font.init(stream)) {
      std::cerr << "standalone font init failed: " << font.lastError() << '\n';
      return 1;
    }
    verifyCompound(font);
  }

  constexpr uint32_t faceOff = 32;
  for (bool otto : {false, true}) {
    VectorStream stream(wrapCollection(buildFont(), faceOff, otto));
    ttf::TtfFont font;
    if (!font.init(stream, faceOff)) {
      std::cerr << (otto ? "OTC OTTO+glyf" : "TTC glyf")
                << " init failed: " << font.lastError() << '\n';
      return 1;
    }
    verifyCompound(font);
  }

  std::cout << "point-attached compound + zero-copy TTC/OTC glyf faces OK\n";
  return 0;
}
