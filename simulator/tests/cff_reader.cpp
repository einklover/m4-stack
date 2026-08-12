#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <utility>
#include <vector>

#include "CffReader.h"

namespace {
void be16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x >> 8); v.push_back(x); }
void be32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x);
}
void put16(std::vector<uint8_t>& v, size_t off, uint16_t x) { v[off] = x >> 8; v[off + 1] = x; }
void put32(std::vector<uint8_t>& v, size_t off, uint32_t x) {
  v[off] = x >> 24; v[off + 1] = x >> 16; v[off + 2] = x >> 8; v[off + 3] = x;
}
void pad4(std::vector<uint8_t>& v) { while (v.size() & 3u) v.push_back(0); }

void dictInt(std::vector<uint8_t>& out, int32_t n) {
  // Keep the fixture deterministic: longint works for every offset we emit.
  out.push_back(29); be32(out, static_cast<uint32_t>(n));
}

void addIndex(std::vector<uint8_t>& cff, const std::vector<std::vector<uint8_t>>& objs) {
  be16(cff, static_cast<uint16_t>(objs.size()));
  if (objs.empty()) return;
  cff.push_back(2);  // offSize
  uint16_t off = 1;
  be16(cff, off);
  for (const auto& o : objs) { off = static_cast<uint16_t>(off + o.size()); be16(cff, off); }
  for (const auto& o : objs) cff.insert(cff.end(), o.begin(), o.end());
}

std::vector<uint8_t> makeCff() {
  std::vector<uint8_t> cff = {1, 0, 4, 2};
  addIndex(cff, {{'M','4','C','F','F'}});  // Name INDEX

  // Top DICT size is fixed because offsets are encoded as 5-byte longints.
  // We build once with placeholders, then derive table-relative offsets.
  std::vector<uint8_t> top;
  dictInt(top, 0); top.push_back(17);                 // CharStrings
  dictInt(top, 2); dictInt(top, 0); top.push_back(18);  // Private size/off
  const size_t topIndexStart = cff.size();
  addIndex(cff, {top});
  addIndex(cff, {});  // String INDEX
  addIndex(cff, {});  // Global Subr INDEX

  const uint32_t charStringsRel = static_cast<uint32_t>(cff.size());
  addIndex(cff, {{14}, {139, 139, 21, 14}});  // .notdef endchar; tiny rmoveto/endchar
  const uint32_t privateRel = static_cast<uint32_t>(cff.size());
  cff.push_back(139); cff.push_back(20);  // defaultWidthX=0

  // Top INDEX layout: count(2), offSize(1), two offsets(4), then dict bytes.
  const size_t dictAbs = topIndexStart + 7;
  put32(cff, dictAbs + 1, charStringsRel);  // after operator 29
  put32(cff, dictAbs + 8, privateRel);      // second longint of Private
  return cff;
}

std::vector<uint8_t> makeOtf(bool corruptCffLength = false) {
  auto cff = makeCff();
  std::vector<uint8_t> otf(12 + 16, 0);
  put32(otf, 0, 0x4f54544f);  // OTTO
  put16(otf, 4, 1);
  std::memcpy(otf.data() + 12, "CFF ", 4);
  const uint32_t off = 28;
  put32(otf, 20, off);
  put32(otf, 24, corruptCffLength ? static_cast<uint32_t>(cff.size() + 4096) : static_cast<uint32_t>(cff.size()));
  otf.insert(otf.end(), cff.begin(), cff.end());
  pad4(otf);
  return otf;
}

class VectorStream final : public ttf::TtfStream {
 public:
  explicit VectorStream(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}
  uint32_t size() const override { return static_cast<uint32_t>(bytes_.size()); }
  bool seek(uint32_t p) override { if (p > bytes_.size()) return false; pos_ = p; return true; }
  uint32_t read(void* dst, uint32_t n) override {
    const uint32_t take = std::min<uint32_t>(n, static_cast<uint32_t>(bytes_.size() - pos_));
    if (take) std::memcpy(dst, bytes_.data() + pos_, take);
    pos_ += take; return take;
  }
 private:
  std::vector<uint8_t> bytes_;
  uint32_t pos_ = 0;
};
}  // namespace

int main() {
  {
    VectorStream s(makeOtf());
    ttf::CffFont font;
    if (!font.init(s)) {
      std::cerr << "CFF init failed: " << font.lastError() << '\n';
      return 1;
    }
    assert(font.ready());
    assert(font.glyphCount() == 2);
    assert(font.charStringsIndex().valid());
    assert(font.globalSubrsIndex().valid());
    assert(font.privateDict().len == 2);
    ttf::CffFont::Slice glyph;
    assert(font.indexObject(font.charStringsIndex(), 1, glyph));
    assert(glyph.len == 4);
  }
  {
    VectorStream s(makeOtf(true));
    ttf::CffFont font;
    assert(!font.init(s));
  }
  std::cout << "streamed CFF1 metadata parser OK\n";
  return 0;
}
