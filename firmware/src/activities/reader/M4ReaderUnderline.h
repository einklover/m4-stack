#pragma once

// Shared native-reader underline placement. The CJK system face can have an
// ascender equal to the row advance, so a clamped underline would land in
// glyph ink. Return -1 when no whitespace row exists and let the caller skip.
inline int m4ReaderUnderlineLineY(int rowTopY, int ascender, int offset, int lineAdvance) {
  if (lineAdvance <= 0) return -1;
  const int wantY = rowTopY + ascender + offset;
  const int nextLineY = rowTopY + lineAdvance - 1;
  if (wantY > nextLineY) return -1;
  return wantY;
}
