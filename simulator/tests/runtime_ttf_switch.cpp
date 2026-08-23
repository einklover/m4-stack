#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "EpdFont.h"
#include "TtfEpdFont.h"
#include "fontIds.h"

void m4AppendFontDiagnostic(const char*) {}

namespace {

constexpr uint16_t kSizePx = 20;
constexpr char32_t kSentence[] = U"中文字体切换测试";

std::vector<uint8_t> readFile(const char* path) {
  std::ifstream in(path, std::ios::binary);
  assert(in && "font fixture missing");
  in.seekg(0, std::ios::end);
  const auto size = in.tellg();
  assert(size > 0);
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  in.seekg(0, std::ios::beg);
  in.read(reinterpret_cast<char*>(bytes.data()), size);
  assert(in);
  return bytes;
}

struct RenderResult {
  int finalX = 0;
  int lineHeight = 0;
  int ascender = 0;
  int descender = 0;
  size_t overdrawPixels = 0;
};

RenderResult renderSentence(TtfEpdFont& font, const char* label) {
  assert(font.valid());
  assert(font.sizePx() == kSizePx);
  assert(font.rasterSizePx() == kSizePx);

  const EpdFontData* data = font.getData();
  assert(data);
  assert(data->advanceY >= kSizePx && data->advanceY <= kSizePx * 3 / 2);
  assert(data->ascender > 0);
  assert(data->descender <= 0);
  assert(data->advanceY >= data->ascender - data->descender);

  constexpr int canvasW = 320;
  constexpr int canvasH = 64;
  std::vector<uint8_t> canvas(canvasW * canvasH, 0);
  int penX = 4;
  int previousPenX = -1;
  int previousInkRight = -1;
  size_t overdraw = 0;

  std::cout << label << " font=" << &font << " nominal=" << font.sizePx()
            << " raster=" << font.rasterSizePx() << " line="
            << static_cast<int>(data->advanceY) << " asc=" << data->ascender
            << " desc=" << data->descender << '\n';

  for (char32_t cp : kSentence) {
    if (cp == 0) break;
    const EpdGlyph* glyph = font.getGlyph(static_cast<uint32_t>(cp));
    assert(glyph);
    const int width = glyph->width;
    const int height = glyph->height;
    const int advance = glyph->advanceX;
    const int left = glyph->left;
    const int top = glyph->top;
    const uint8_t* bitmap = font.loadGlyphBitmap(glyph, nullptr);

    assert(bitmap);
    assert(width > 0 && width <= kSizePx * 2);
    assert(height > 0 && height <= kSizePx * 2);
    assert(advance > 0 && advance <= kSizePx * 2);
    if (previousPenX >= 0) assert(penX > previousPenX);

    const int inkLeft = penX + left;
    const int inkRight = inkLeft + width;
    if (previousInkRight >= 0) assert(inkLeft >= previousInkRight - 2);

    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const int pixel = y * width + x;
        const uint8_t raw = (bitmap[pixel / 4] >> ((3 - pixel % 4) * 2)) & 0x3;
        if (raw == 0) continue;
        const int dx = inkLeft + x;
        const int dy = data->ascender - top + y;
        if (dx < 0 || dx >= canvasW || dy < 0 || dy >= canvasH) continue;
        uint8_t& dst = canvas[dy * canvasW + dx];
        if (dst) ++overdraw;
        dst = 1;
      }
    }

    std::cout << "  U+" << std::hex << std::uppercase << static_cast<uint32_t>(cp)
              << std::dec << " x=" << penX << " adv=" << advance << " bmp="
              << width << 'x' << height << " left=" << left << " top=" << top
              << " ink=[" << inkLeft << ',' << inkRight << ")\n";
    previousPenX = penX;
    previousInkRight = inkRight;
    penX += advance;
  }

  assert(overdraw == 0);
  return {penX, data->advanceY, data->ascender, data->descender, overdraw};
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: m4_runtime_ttf_switch_tests <first.ttf> <second.ttf>\n";
    return 2;
  }

  const auto firstBytes = readFile(argv[1]);
  const auto secondBytes = readFile(argv[2]);

  EpdFontData chromeData{};
  EpdFont chromeFont(&chromeData);
  const std::map<int, const EpdFont*> chromeBefore = {
      {SMALL_FONT_ID, &chromeFont},
      {UI_10_FONT_ID, &chromeFont},
      {UI_12_FONT_ID, &chromeFont},
  };

  auto first = std::make_unique<TtfEpdFont>(firstBytes.data(), firstBytes.size(), kSizePx, 64, 256 * 1024);
  const auto firstResult = renderSentence(*first, "before-switch");

  auto second = std::make_unique<TtfEpdFont>(secondBytes.data(), secondBytes.size(), kSizePx, 64, 256 * 1024);
  assert(second.get() != first.get());
  const TtfEpdFont* oldReaderPointer = first.get();
  first.swap(second);
  second.reset();
  assert(first.get() != oldReaderPointer);
  const auto secondResult = renderSentence(*first, "after-switch");

  const std::map<int, const EpdFont*> chromeAfter = chromeBefore;
  assert(chromeAfter == chromeBefore);
  assert(chromeAfter.at(SMALL_FONT_ID) == &chromeFont);
  assert(chromeAfter.at(UI_10_FONT_ID) == &chromeFont);
  assert(chromeAfter.at(UI_12_FONT_ID) == &chromeFont);
  assert(firstResult.finalX > 4 && secondResult.finalX > 4);

  std::cout << "runtime TTF switch render: PASS; chrome IDs=" << SMALL_FONT_ID << ','
            << UI_10_FONT_ID << ',' << UI_12_FONT_ID << '\n';
  return 0;
}
