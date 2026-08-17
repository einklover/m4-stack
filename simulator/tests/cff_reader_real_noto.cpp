#include <cassert>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

#include "CffReader.h"

namespace {

uint16_t be16(const uint8_t* p) {
  return static_cast<uint16_t>((uint16_t(p[0]) << 8) | p[1]);
}

uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

class FileStream final : public ttf::TtfStream {
 public:
  explicit FileStream(const char* path) : file_(path, std::ios::binary) {
    if (!file_) return;
    file_.seekg(0, std::ios::end);
    const std::streamoff end = file_.tellg();
    if (end < 0 || static_cast<uint64_t>(end) > 0xffffffffu) {
      file_.close();
      return;
    }
    size_ = static_cast<uint32_t>(end);
    file_.seekg(0, std::ios::beg);
  }

  bool valid() const { return file_.is_open() && size_ > 0; }
  uint32_t size() const override { return size_; }

  bool seek(uint32_t pos) override {
    if (!file_.is_open() || pos > size_) return false;
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(pos), std::ios::beg);
    return static_cast<bool>(file_);
  }

  uint32_t read(void* dst, uint32_t n) override {
    if (!file_.is_open() || !dst || n == 0) return 0;
    file_.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
    return static_cast<uint32_t>(file_.gcount());
  }

 private:
  std::ifstream file_;
  uint32_t size_ = 0;
};

bool readAt(ttf::TtfStream& stream, uint32_t off, void* dst, uint32_t n) {
  return off <= stream.size() && n <= stream.size() - off && stream.seek(off) &&
         stream.read(dst, n) == n;
}

bool collectionFaceOffset(ttf::TtfStream& stream, uint32_t faceIndex, uint32_t& out) {
  out = 0;
  uint8_t header[12] = {};
  if (!readAt(stream, 0, header, sizeof(header)) || std::memcmp(header, "ttcf", 4) != 0) {
    return false;
  }
  const uint32_t count = be32(header + 8);
  if (faceIndex >= count || count == 0 || count > 64 ||
      12u + uint64_t(count) * 4u > stream.size()) {
    return false;
  }
  uint8_t raw[4] = {};
  if (!readAt(stream, 12u + faceIndex * 4u, raw, sizeof(raw))) return false;
  out = be32(raw);
  return out < stream.size();
}

bool hasInk(const ttf::GlyphBitmap& bitmap) {
  if (!bitmap.data || bitmap.packedLen == 0) return false;
  for (uint16_t i = 0; i < bitmap.packedLen; ++i) {
    if (bitmap.data[i] != 0) return true;
  }
  return false;
}

void checkGlyph(ttf::CffFont& font, uint32_t cp) {
  uint16_t gid = 0;
  if (!font.findGlyph(cp, gid) || gid == 0) {
    std::cerr << "missing glyph U+" << std::hex << cp << std::dec << "\n";
    std::abort();
  }

  int32_t advanceUnits = 0;
  int32_t lsbUnits = 0;
  if (!font.glyphHMetrics(gid, advanceUnits, lsbUnits) || advanceUnits <= 0) {
    std::cerr << "bad metrics U+" << std::hex << cp << std::dec << " gid=" << gid << "\n";
    std::abort();
  }

  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  if (!font.glyphPixelBox(gid, 32, x0, y0, x1, y1) || x1 <= x0 || y1 <= y0) {
    std::cerr << "bad pixel box U+" << std::hex << cp << std::dec << " gid=" << gid << "\n";
    std::abort();
  }

  ttf::GlyphBitmap bitmap;
  if (!font.rasterize(gid, 32, bitmap) || bitmap.width <= 0 || bitmap.height <= 0 ||
      bitmap.advance <= 0 || !hasInk(bitmap)) {
    std::cerr << "raster failed U+" << std::hex << cp << std::dec << " gid=" << gid
              << " err=" << font.lastError() << "\n";
    std::abort();
  }

  std::cout << "U+" << std::hex << cp << std::dec << " gid=" << gid
            << " box=" << bitmap.width << "x" << bitmap.height
            << " advance=" << bitmap.advance << " packed=" << bitmap.packedLen << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: m4_cff_real_cjk_tests <NotoSansCJK-Regular.ttc> <face-index>\n";
    return 2;
  }

  const uint32_t faceIndex = static_cast<uint32_t>(std::stoul(argv[2]));
  FileStream stream(argv[1]);
  if (!stream.valid()) {
    std::cerr << "cannot open real CJK font fixture: " << argv[1] << "\n";
    return 2;
  }

  uint32_t faceOffset = 0;
  if (!collectionFaceOffset(stream, faceIndex, faceOffset)) {
    std::cerr << "cannot resolve TTC/OTC face index " << faceIndex << "\n";
    return 2;
  }

  ttf::CffFont font;
  if (!font.init(stream, faceOffset)) {
    std::cerr << "real Noto CID-CFF init failed: " << font.lastError() << "\n";
    return 1;
  }

  // Debian/Ubuntu NotoSansCJK-Regular.ttc face 2 is Noto Sans CJK SC.
  // The assertions intentionally exercise a real Adobe-Identity CID-keyed CFF
  // with FDSelect format 3 and per-FD local Subrs, not a synthetic fixture.
  assert(font.ready());
  assert(font.isCidKeyed());
  assert(font.fdCount() > 1);
  assert(font.glyphCount() > 60000);
  assert(font.unitsPerEm() > 0);

  checkGlyph(font, 0x56FD);  // 国
  checkGlyph(font, 0x4E2D);  // 中
  checkGlyph(font, 0x6C38);  // 永; exercises local Subrs in Noto CJK
  checkGlyph(font, 0x3002);  // 。
  checkGlyph(font, 0xFF0C);  // ，

  std::cout << "real Noto Sans CJK SC CID-CFF TTC face " << faceIndex
            << " offset=" << faceOffset << " fdCount=" << font.fdCount()
            << " glyphs=" << font.glyphCount() << " OK\n";
  return 0;
}
