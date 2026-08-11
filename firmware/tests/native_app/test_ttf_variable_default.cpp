#include "TtfReader.h"

#include <cassert>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

void be16(std::vector<uint8_t>& out, size_t pos, uint16_t v) {
  out[pos] = static_cast<uint8_t>(v >> 8);
  out[pos + 1] = static_cast<uint8_t>(v);
}

void be32(std::vector<uint8_t>& out, size_t pos, uint32_t v) {
  out[pos] = static_cast<uint8_t>(v >> 24);
  out[pos + 1] = static_cast<uint8_t>(v >> 16);
  out[pos + 2] = static_cast<uint8_t>(v >> 8);
  out[pos + 3] = static_cast<uint8_t>(v);
}

std::vector<uint8_t> minimalHead() {
  std::vector<uint8_t> t(54, 0);
  be16(t, 18, 1000);  // unitsPerEm
  be16(t, 42, 900);   // yMax
  be16(t, 50, 0);     // short loca
  return t;
}

std::vector<uint8_t> minimalMaxp() {
  std::vector<uint8_t> t(6, 0);
  be32(t, 0, 0x00010000u);
  be16(t, 4, 1);  // .notdef only
  return t;
}

std::vector<uint8_t> minimalHhea() {
  std::vector<uint8_t> t(36, 0);
  be16(t, 4, 900);
  be16(t, 6, static_cast<uint16_t>(-100));
  be16(t, 34, 1);
  return t;
}

std::vector<uint8_t> minimalCmap() {
  // cmap header + one Windows BMP encoding record + a one-segment format-4
  // subtable containing only the required 0xFFFF sentinel.
  std::vector<uint8_t> t(12 + 24, 0);
  be16(t, 2, 1);      // one encoding record
  be16(t, 4, 3);      // Windows
  be16(t, 6, 1);      // Unicode BMP
  be32(t, 8, 12);     // subtable offset
  const size_t s = 12;
  be16(t, s + 0, 4);  // format 4
  be16(t, s + 2, 24); // length
  be16(t, s + 6, 2);  // segCountX2 = 2 => one segment
  be16(t, s + 8, 2);  // searchRange
  be16(t, s + 14, 0xFFFF); // endCode[0]
  be16(t, s + 18, 0xFFFF); // startCode[0]
  be16(t, s + 20, 1);      // idDelta => .notdef
  be16(t, s + 22, 0);      // idRangeOffset
  return t;
}

struct TableDef {
  const char* tag;
  std::vector<uint8_t> data;
};

std::vector<uint8_t> buildVariableTtf() {
  std::vector<TableDef> tables = {
      {"head", minimalHead()},
      {"maxp", minimalMaxp()},
      {"loca", std::vector<uint8_t>(4, 0)},
      {"cmap", minimalCmap()},
      {"hhea", minimalHhea()},
      {"hmtx", std::vector<uint8_t>{0x03, 0xE8, 0x00, 0x00}},
      {"glyf", std::vector<uint8_t>{0x00}},
      // Presence of these two tables used to reject an otherwise valid glyf
      // TrueType file. M4 now renders the default/base instance and ignores
      // variation axes/deltas until axis selection is implemented.
      {"fvar", std::vector<uint8_t>{0x00}},
      {"gvar", std::vector<uint8_t>{0x00}},
  };

  const uint16_t count = static_cast<uint16_t>(tables.size());
  std::vector<uint8_t> out(12u + static_cast<size_t>(count) * 16u, 0);
  be32(out, 0, 0x00010000u);
  be16(out, 4, count);

  size_t cursor = out.size();
  for (size_t i = 0; i < tables.size(); ++i) {
    while ((cursor & 3u) != 0) {
      out.push_back(0);
      ++cursor;
    }
    const size_t rec = 12u + i * 16u;
    be32(out, rec, (static_cast<uint32_t>(tables[i].tag[0]) << 24) |
                       (static_cast<uint32_t>(tables[i].tag[1]) << 16) |
                       (static_cast<uint32_t>(tables[i].tag[2]) << 8) |
                       static_cast<uint32_t>(tables[i].tag[3]));
    be32(out, rec + 8, static_cast<uint32_t>(cursor));
    be32(out, rec + 12, static_cast<uint32_t>(tables[i].data.size()));
    out.insert(out.end(), tables[i].data.begin(), tables[i].data.end());
    cursor += tables[i].data.size();
  }
  return out;
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
    if (!dst || pos_ >= bytes_.size()) return 0;
    const size_t left = bytes_.size() - pos_;
    const size_t take = n < left ? n : left;
    auto* p = static_cast<uint8_t*>(dst);
    for (size_t i = 0; i < take; ++i) p[i] = bytes_[pos_ + i];
    pos_ += take;
    return static_cast<uint32_t>(take);
  }
 private:
  std::vector<uint8_t> bytes_;
  size_t pos_ = 0;
};

}  // namespace

int main() {
  VectorStream stream(buildVariableTtf());
  ttf::TtfFont font;
  assert(font.init(stream));
  assert(std::string(font.lastError()) == "ok");
  assert(font.ready());
  assert(font.unitsPerEm() == 1000);
  return 0;
}
