#pragma once

namespace M4NativeProviderDetailTouchPolicy {

enum class Action { None, Back, Read, Chapter };

struct Layout {
  int screenWidth = 0;
  int screenHeight = 0;
  int footerTop = 0;
  int readTop = 0;
  int readHeight = 0;
  int chapterTop = 0;
  int chapterBottom = 0;

  void reset(int width, int height, int footerHeight) {
    screenWidth = width > 0 ? width : 0;
    screenHeight = height > 0 ? height : 0;
    const int safeFooterHeight = footerHeight > 0 ? footerHeight : 0;
    footerTop = screenHeight > safeFooterHeight ? screenHeight - safeFooterHeight : 0;
    readTop = readHeight = chapterTop = chapterBottom = 0;
  }

  void setReadButton(int top, int height) {
    readTop = top;
    readHeight = height > 0 ? height : 0;
  }

  void setChapterBlock(int top, int bottom) {
    chapterTop = top;
    chapterBottom = bottom > top ? bottom : top;
  }

  Action actionAt(int x, int y) const {
    if (readHeight > 0 && y >= readTop && y < readTop + readHeight) return Action::Read;
    if (x >= 0 && x < screenWidth && y >= footerTop && y < screenHeight) return Action::Back;
    if (chapterBottom > chapterTop && y >= chapterTop && y < chapterBottom) return Action::Chapter;
    return Action::None;
  }
};

}  // namespace M4NativeProviderDetailTouchPolicy
