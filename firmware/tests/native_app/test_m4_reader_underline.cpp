// M4 native-reader underline placement contract (P0-1).
//
// RED until the shared TxtReaderActivity underline stops clamping into glyph
// ink: the CJK system face has ascender == advanceY and descender == 0, so at
// default spacing wantY = y + ascender + offset NEVER fits between rows and
// the old min() clamp pinned a dashed line into ink on every line (both
// provider plugins flow through this one renderer).
// Contract: m4ReaderUnderlineLineY returns the drawable y only when it fits
// strictly above the next row top; otherwise -1 and the caller skips instead
// of drawing through text. Fitted positions are byte-identical to before.
//
// Build:
//   /opt/homebrew/bin/g++-14 -std=c++17 -I firmware/src \
//     firmware/tests/native_app/test_m4_reader_underline.cpp \
//     -o /tmp/test_m4_reader_underline && /tmp/test_m4_reader_underline
// Run from the workspace root (/tmp/m4-ui-redesign-apple-phase1).

#include <cassert>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "activities/reader/M4ReaderUnderline.h"

namespace {

std::string readFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

}  // namespace

int main() {
  // CJK defaults (CenterKernel: ascender == advance, descender 0, offset 5):
  // wantY = y+21 overshoots nextLineY = y+15 -> no ink-free pixel -> skip.
  // The old min() clamp drew at y+15, i.e. through glyph ink on every row.
  assert(m4ReaderUnderlineLineY(100, 16, 5, 16) == -1);
  assert(m4ReaderUnderlineLineY(0, 16, 5, 16) == -1);

  // Latin-ish metrics with real leading: fits, position unchanged.
  assert(m4ReaderUnderlineLineY(100, 12, 5, 24) == 117);

  // Wide spacing restores the decoration for CJK too.
  assert(m4ReaderUnderlineLineY(100, 16, 5, 24) == 121);

  // Degenerate advance: never draw.
  assert(m4ReaderUnderlineLineY(100, 16, 5, 0) == -1);
  assert(m4ReaderUnderlineLineY(100, 16, 5, -3) == -1);

  // Wiring: the shared renderer must use the helper (one fix, both plugins).
  const std::string src =
      readFile("firmware/src/activities/reader/TxtReaderActivity.cpp");
  assert(!src.empty());
  assert(src.find("m4ReaderUnderlineLineY") != std::string::npos);

  std::puts("READER_UNDERLINE_CONTRACT_OK");
  return 0;
}
