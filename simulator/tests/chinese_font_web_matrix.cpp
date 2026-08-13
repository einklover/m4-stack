#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

#include "CffReader.h"
#include "TtfReader.h"

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
    if (end <= 0 || static_cast<uint64_t>(end) > 0xffffffffu) {
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

bool resolveFaceOffset(ttf::TtfStream& stream, uint32_t faceIndex, uint32_t& out) {
  out = 0;
  uint8_t header[12] = {};
  if (!readAt(stream, 0, header, sizeof(header))) return false;
  if (std::memcmp(header, "ttcf", 4) != 0) return faceIndex == 0;

  const uint32_t count = be32(header + 8);
  if (count == 0 || count > 128 || faceIndex >= count ||
      12u + uint64_t(count) * 4u > stream.size()) {
    return false;
  }
  uint8_t raw[4] = {};
  if (!readAt(stream, 12u + faceIndex * 4u, raw, sizeof(raw))) return false;
  out = be32(raw);
  return out < stream.size();
}

bool hasTable(ttf::TtfStream& stream, uint32_t faceOffset, const char tag[4]) {
  uint8_t sfnt[12] = {};
  if (!readAt(stream, faceOffset, sfnt, sizeof(sfnt))) return false;
  const uint16_t count = be16(sfnt + 4);
  if (count == 0 || count > 256 ||
      uint64_t(faceOffset) + 12u + uint64_t(count) * 16u > stream.size()) {
    return false;
  }
  for (uint16_t i = 0; i < count; ++i) {
    uint8_t record[16] = {};
    if (!readAt(stream, faceOffset + 12u + uint32_t(i) * 16u, record, sizeof(record))) {
      return false;
    }
    if (std::memcmp(record, tag, 4) == 0) return true;
  }
  return false;
}

bool hasInk(const ttf::GlyphBitmap& bitmap) {
  if (!bitmap.data || bitmap.packedLen == 0) return false;
  for (uint16_t i = 0; i < bitmap.packedLen; ++i) {
    if (bitmap.data[i] != 0) return true;
  }
  return false;
}

template <typename Font>
bool checkGlyph(Font& font, uint32_t cp, uint16_t sizePx) {
  uint16_t gid = 0;
  if (!font.findGlyph(cp, gid) || gid == 0) {
    std::cerr << "missing glyph U+" << std::hex << cp << std::dec << "\n";
    return false;
  }

  int32_t advanceUnits = 0;
  int32_t lsbUnits = 0;
  if (!font.glyphHMetrics(gid, advanceUnits, lsbUnits) || advanceUnits <= 0) {
    std::cerr << "bad metrics U+" << std::hex << cp << std::dec << " gid=" << gid << "\n";
    return false;
  }

  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  if (!font.glyphPixelBox(gid, sizePx, x0, y0, x1, y1) || x1 <= x0 || y1 <= y0) {
    std::cerr << "bad pixel box U+" << std::hex << cp << std::dec << " gid=" << gid
              << " size=" << sizePx << "\n";
    return false;
  }

  ttf::GlyphBitmap bitmap;
  if (!font.rasterize(gid, sizePx, bitmap) || bitmap.width <= 0 || bitmap.height <= 0 ||
      bitmap.advance <= 0 || !hasInk(bitmap)) {
    std::cerr << "raster failed U+" << std::hex << cp << std::dec << " gid=" << gid
              << " size=" << sizePx << " err=" << font.lastError() << "\n";
    return false;
  }

  std::cout << " U+" << std::hex << cp << std::dec << "@" << sizePx << "px"
            << " gid=" << gid << " box=" << bitmap.width << "x" << bitmap.height
            << " adv=" << bitmap.advance << " packed=" << bitmap.packedLen << "\n";
  return true;
}

template <typename Font>
bool exerciseChinese(Font& font) {
  static constexpr uint32_t kChinese[] = {
      0x4E2D,  // 中
      0x56FD,  // 国
      0x6C49,  // 汉
      0x5B57,  // 字
      0x6C38,  // 永
      0x4E66,  // 书
      0x9605,  // 阅
      0x8BFB,  // 读
  };
  static constexpr uint16_t kSizes[] = {20, 32};
  for (uint16_t sizePx : kSizes) {
    for (uint32_t cp : kChinese) {
      if (!checkGlyph(font, cp, sizePx)) return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: m4_web_chinese_font_tests <font.ttf|font.otf|font.ttc> <face-index>\n";
    return 2;
  }

  uint32_t faceIndex = 0;
  try {
    faceIndex = static_cast<uint32_t>(std::stoul(argv[2]));
  } catch (...) {
    std::cerr << "invalid face index: " << argv[2] << "\n";
    return 2;
  }

  FileStream stream(argv[1]);
  if (!stream.valid()) {
    std::cerr << "cannot open font: " << argv[1] << "\n";
    return 2;
  }

  uint32_t faceOffset = 0;
  if (!resolveFaceOffset(stream, faceIndex, faceOffset)) {
    std::cerr << "cannot resolve face " << faceIndex << " in " << argv[1] << "\n";
    return 2;
  }

  const bool cff1 = hasTable(stream, faceOffset, "CFF ");
  const bool cff2 = hasTable(stream, faceOffset, "CFF2");
  const bool glyf = hasTable(stream, faceOffset, "glyf") && hasTable(stream, faceOffset, "loca");
  const bool variable = hasTable(stream, faceOffset, "fvar") || hasTable(stream, faceOffset, "gvar");

  std::cout << "font=" << argv[1] << " face=" << faceIndex << " offset=" << faceOffset
            << " backend=" << (cff1 ? "CFF1" : (glyf ? "glyf" : (cff2 ? "CFF2" : "unknown")))
            << " variable=" << (variable ? "yes" : "no") << "\n";

  if (cff2) {
    std::cerr << "CFF2 outline is not supported by the current runtime parser\n";
    return 1;
  }

  if (cff1) {
    ttf::CffFont font;
    if (!font.init(stream, faceOffset)) {
      std::cerr << "CFF init failed: " << font.lastError() << "\n";
      return 1;
    }
    if (!font.ready() || font.unitsPerEm() == 0 || font.glyphCount() == 0) {
      std::cerr << "CFF font did not become ready\n";
      return 1;
    }
    if (!exerciseChinese(font)) return 1;
    std::cout << "PASS CFF1 glyphs=" << font.glyphCount()
              << " cid=" << (font.isCidKeyed() ? "yes" : "no")
              << " fdCount=" << font.fdCount() << "\n";
    return 0;
  }

  if (glyf) {
    ttf::TtfFont font;
    if (!font.init(stream, faceOffset)) {
      std::cerr << "TTF init failed: " << font.lastError() << "\n";
      return 1;
    }
    if (!font.ready() || font.unitsPerEm() == 0 || font.numGlyphs() <= 0) {
      std::cerr << "TTF font did not become ready\n";
      return 1;
    }
    if (!exerciseChinese(font)) return 1;
    std::cout << "PASS glyf glyphs=" << font.numGlyphs()
              << " variable-default=" << (variable ? "yes" : "no") << "\n";
    return 0;
  }

  std::cerr << "unsupported outline tables (need CFF1 or glyf/loca)\n";
  return 1;
}
