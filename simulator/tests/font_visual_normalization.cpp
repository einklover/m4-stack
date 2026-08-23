#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "CffReader.h"
#include "TtfReader.h"
#include "TtfVisualNormalization.h"

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
  explicit FileStream(const std::string& path) : file_(path, std::ios::binary) {
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

  bool valid() const { return file_.is_open() && size_ != 0; }
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

bool resolveFaceOffset(ttf::TtfStream& stream, uint32_t& faceOffset) {
  faceOffset = 0;
  uint8_t header[12] = {};
  if (!readAt(stream, 0, header, sizeof(header))) return false;
  if (std::memcmp(header, "ttcf", 4) != 0) return true;
  const uint32_t count = be32(header + 8);
  if (count == 0 || count > 64 || 12u + uint64_t(count) * 4u > stream.size()) return false;
  uint8_t raw[4] = {};
  if (!readAt(stream, 12, raw, sizeof(raw))) return false;
  faceOffset = be32(raw);
  return faceOffset < stream.size();
}

enum class Backend { Glyf, Cff1, Cff2, Unknown };

Backend detectBackend(ttf::TtfStream& stream, uint32_t faceOffset) {
  uint8_t header[12] = {};
  if (!readAt(stream, faceOffset, header, sizeof(header))) return Backend::Unknown;
  const uint32_t signature = be32(header);
  const uint16_t tableCount = be16(header + 4);
  if ((signature != 0x00010000u && signature != 0x74727565u && signature != 0x4f54544fu) ||
      tableCount == 0 || tableCount > 128 ||
      uint64_t(faceOffset) + 12u + uint64_t(tableCount) * 16u > stream.size()) {
    return Backend::Unknown;
  }

  bool glyf = false, loca = false, cff = false, cff2 = false;
  for (uint16_t i = 0; i < tableCount; ++i) {
    uint8_t record[16] = {};
    if (!readAt(stream, faceOffset + 12u + uint32_t(i) * 16u, record, sizeof(record))) {
      return Backend::Unknown;
    }
    const uint32_t tag = be32(record);
    const uint32_t off = be32(record + 8);
    const uint32_t len = be32(record + 12);
    if (off > stream.size() || len > stream.size() - off) return Backend::Unknown;
    glyf |= tag == 0x676c7966u;
    loca |= tag == 0x6c6f6361u;
    cff |= tag == 0x43464620u;
    cff2 |= tag == 0x43464632u;
  }
  if (cff2) return Backend::Cff2;
  if (cff) return Backend::Cff1;
  if (glyf && loca) return Backend::Glyf;
  return Backend::Unknown;
}

const char* backendName(Backend backend) {
  switch (backend) {
    case Backend::Glyf: return "glyf";
    case Backend::Cff1: return "CFF1";
    case Backend::Cff2: return "CFF2";
    default: return "unknown";
  }
}

struct Sample {
  uint32_t cp = 0;
  uint16_t gid = 0;
  int width = 0;
  int height = 0;
  int x0 = 0;
  int y0 = 0;
  int x1 = 0;
  int y1 = 0;
  int32_t advanceUnits = 0;
};

float medianHeight(const std::vector<Sample>& samples) {
  std::vector<int> heights;
  heights.reserve(samples.size());
  for (const Sample& sample : samples) heights.push_back(sample.height);
  if (heights.empty()) return 0.0f;
  std::sort(heights.begin(), heights.end());
  const size_t middle = heights.size() / 2;
  if (heights.size() & 1u) return static_cast<float>(heights[middle]);
  return 0.5f * static_cast<float>(heights[middle - 1] + heights[middle]);
}

template <typename Font>
bool collectSamples(Font& font, uint16_t sizePx, std::vector<Sample>& out) {
  out.clear();
  for (size_t i = 0; i < 6; ++i) {
    const uint32_t cp = M4TtfVisualNormalization::kReferenceCodepoints[i];
    uint16_t gid = 0;
    if (!font.findGlyph(cp, gid) || gid == 0) continue;
    int32_t advanceUnits = 0, lsbUnits = 0;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (!font.glyphHMetrics(gid, advanceUnits, lsbUnits) || advanceUnits <= 0 ||
        !font.glyphPixelBox(gid, sizePx, x0, y0, x1, y1) || x1 <= x0 || y1 <= y0) {
      continue;
    }
    out.push_back({cp, gid, x1 - x0, y1 - y0, x0, y0, x1, y1, advanceUnits});
  }
  if (!out.empty()) return true;

  for (size_t i = 6; i < M4TtfVisualNormalization::kReferenceCodepointCount; ++i) {
    const uint32_t cp = M4TtfVisualNormalization::kReferenceCodepoints[i];
    uint16_t gid = 0;
    if (!font.findGlyph(cp, gid) || gid == 0) continue;
    int32_t advanceUnits = 0, lsbUnits = 0;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (!font.glyphHMetrics(gid, advanceUnits, lsbUnits) || advanceUnits <= 0 ||
        !font.glyphPixelBox(gid, sizePx, x0, y0, x1, y1) || x1 <= x0 || y1 <= y0) {
      continue;
    }
    out.push_back({cp, gid, x1 - x0, y1 - y0, x0, y0, x1, y1, advanceUnits});
    break;
  }
  return !out.empty();
}

template <typename Font>
bool measureAt(Font& font, uint16_t nominalPx, const std::string& name,
               Backend backend, bool printRow) {
  std::vector<Sample> before;
  if (!collectSamples(font, nominalPx, before)) {
    std::cerr << name << " has no usable visual reference\n";
    return false;
  }

  const Sample& anchor = before.front();
  const float robustBefore = medianHeight(before);
  const float visualScale = M4TtfVisualNormalization::scaleForReference(nominalPx, robustBefore);
  const uint16_t rasterPx = M4TtfVisualNormalization::renderPixelSize(nominalPx, visualScale);

  std::vector<Sample> after;
  after.reserve(before.size());
  for (const Sample& sample : before) {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (!font.glyphPixelBox(sample.gid, rasterPx, x0, y0, x1, y1) || x1 <= x0 || y1 <= y0) {
      std::cerr << name << " failed normalized bbox U+" << std::hex << sample.cp << std::dec << "\n";
      return false;
    }
    Sample normalized = sample;
    normalized.x0 = x0;
    normalized.y0 = y0;
    normalized.x1 = x1;
    normalized.y1 = y1;
    normalized.width = x1 - x0;
    normalized.height = y1 - y0;
    after.push_back(normalized);
  }

  int32_t anchorLsb = 0;
  int32_t anchorAdvanceUnits = 0;
  if (!font.glyphHMetrics(anchor.gid, anchorAdvanceUnits, anchorLsb)) return false;
  const int nominalAdvance = std::max(
      1, std::min(255, static_cast<int>(std::lround(
          static_cast<float>(anchorAdvanceUnits) * nominalPx / font.unitsPerEm()))));

  ttf::GlyphBitmap anchorBitmap;
  if (!font.rasterize(anchor.gid, rasterPx, anchorBitmap) || anchorBitmap.width <= 0 ||
      anchorBitmap.height <= 0) {
    std::cerr << name << " failed normalized raster\n";
    return false;
  }
  const int visualOrigin = static_cast<int>(std::lround(
      (static_cast<float>(nominalAdvance) - 2.0f * anchorBitmap.xoff - anchorBitmap.width) * 0.5f));
  const float centerError = anchorBitmap.xoff + visualOrigin + anchorBitmap.width * 0.5f -
                            nominalAdvance * 0.5f;
  const float robustAfter = medianHeight(after);
  const float target = M4TtfVisualNormalization::targetReferenceHeight(nominalPx);

  if (std::fabs(robustAfter - target) > 1.5f || std::fabs(centerError) > 1.0f ||
      anchorBitmap.width <= 0 || anchorBitmap.width > nominalPx * 2 + 2 ||
      nominalAdvance <= 0 || nominalAdvance > nominalPx * 2 + 2 || nominalAdvance > 255) {
    std::cerr << name << " normalization failed: before=" << robustBefore
              << " after=" << robustAfter << " target=" << target
              << " center=" << centerError << " advance=" << nominalAdvance << "\n";
    return false;
  }

  if (printRow) {
    std::cout << std::left << std::setw(42) << name.substr(0, 41)
              << " " << backendName(backend)
              << " ref=U+" << std::hex << std::uppercase << anchor.cp << std::dec
              << " before=" << anchor.width << "x" << anchor.height
              << "/med" << robustBefore
              << " after=" << anchorBitmap.width << "x" << anchorBitmap.height
              << "/med" << robustAfter
              << " target=" << target
              << " scale=" << std::fixed << std::setprecision(3) << visualScale
              << " raster=" << rasterPx
              << " center=" << std::setprecision(2) << centerError
              << " adv=" << nominalAdvance << "\n";
  }
  return true;
}

template <typename Font>
bool runFont(Font& font, const std::string& name, Backend backend) {
  if (!font.ready() || font.unitsPerEm() == 0) return false;
  if (!measureAt(font, 20, name, backend, true)) return false;
  return measureAt(font, 33, name, backend, false);
}

bool runOne(const std::string& path, const std::string& name) {
  FileStream stream(path);
  if (!stream.valid()) return false;
  uint32_t faceOffset = 0;
  if (!resolveFaceOffset(stream, faceOffset)) return false;
  const Backend backend = detectBackend(stream, faceOffset);
  if (backend == Backend::Glyf) {
    ttf::TtfFont font;
    return font.init(stream, faceOffset) && runFont(font, name, backend);
  }
  if (backend == Backend::Cff1 || backend == Backend::Cff2) {
    ttf::CffFont font;
    return font.init(stream, faceOffset) && runFont(font, name, backend);
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: m4_font_visual_normalization_tests <Downloads-font-folder>\n";
    return 2;
  }

  static constexpr const char* kSamples[] = {
      "FZYTJW.TTF",
      "ChillReunion_Round.otf",
      "ChillReunion_Sans.otf",
      "思源宋体-Regular.otf",
      "思源宋体SC-VF.ttf",
      "HarmonyOSSans.ttf",
      "SourceHanSerifSC-Medium.ttf",
      "文泉驿点阵宋体.ttf",
      "ELEYANG-Bonjour-信达雅之书ExtraBold.ttf",
      "柯西.ttf",
      "方正像素24.ttf",
      "小猫的理想是小鱼干.ttf",
      "kindle自带圆体.ttf",
      "FZWBJW.TTF",
      "FZYuShiNanKaiSJ.TTF",
  };

  const std::string root = argv[1];
  size_t passed = 0;
  size_t missing = 0;
  std::cout << "font visual normalization matrix @20px (target reference height "
            << M4TtfVisualNormalization::targetReferenceHeight(20) << ")\n";
  for (const char* sample : kSamples) {
    const std::string name(sample);
    const std::string path = root + "/" + name;
    std::ifstream probe(path, std::ios::binary);
    if (!probe) {
      ++missing;
      continue;
    }
    if (!runOne(path, name)) {
      std::cerr << "FAIL " << name << "\n";
      return 1;
    }
    ++passed;
  }

  if (passed < 10) {
    std::cerr << "need at least 10 sampled fonts; passed=" << passed
              << " missing=" << missing << "\n";
    return 2;
  }
  std::cout << "font visual normalization matrix: PASS fonts=" << passed
            << " missing=" << missing << "\n";
  return 0;
}
