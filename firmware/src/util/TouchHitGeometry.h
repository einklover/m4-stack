#pragma once

// Shared pure geometry for drawing + touch hit-testing (host-testable).
// Keep free of Arduino/hardware so native/host tests can exercise them.

#include "HomeRef.h"

namespace TouchHitGeometry {

inline int minInt(int a, int b) { return a < b ? a : b; }
inline int maxInt(int a, int b) { return a > b ? a : b; }
inline int ceilDiv(int num, int den) { return den <= 0 ? 0 : (num + den - 1) / den; }

struct Rect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;

  bool contains(int px, int py) const {
    return px >= x && px < x + width && py >= y && py < y + height;
  }
};

// Shared system chrome geometry. Drawing and touch routing must use the same
// rectangles; keep these values host-testable and independent of Arduino.
struct BottomNavigationLayout {
  Rect back;
  Rect home;
  Rect menu;

  bool valid() const { return back.width > 0 && back.height > 0 && home.width > 0 && home.height > 0; }
};

inline BottomNavigationLayout makeBottomNavigationLayout(int screenWidth, int screenHeight,
                                                         int barHeight = HomeRef::BottomH) {
  if (screenWidth <= 1 || screenHeight <= 0 || barHeight <= 0 || barHeight > screenHeight) return {};
  const int y = screenHeight - barHeight;
  int split1 = HomeRef::BottomSplit1;
  int split2 = HomeRef::BottomSplit2;
  if (screenWidth != HomeRef::ScreenW) {
    split1 = screenWidth / 3;
    split2 = (screenWidth * 2) / 3;
  }
  return {{0, y, split1, barHeight},
          {split1, y, split2 - split1, barHeight},
          {split2, y, screenWidth - split2, barHeight}};
}

// Chapter selectors reserve the first 65px for the title/list boundary, so a
// 128x64 target makes the visible top Back control easy to hit without taking
// any chapter row space.
constexpr int kChapterHeaderBackWidth = 128;
constexpr int kChapterHeaderBackHeight = 64;

inline Rect chapterHeaderBackRect(int x = 0, int y = 0) {
  return {x, y, kChapterHeaderBackWidth, kChapterHeaderBackHeight};
}


// ---------------------------------------------------------------------------
// Fengyan reference-style recent books: one hero row plus three compact books.
// Geometry is shared by drawing and touch routing so the e-ink UI never
// develops a visible/tappable mismatch.
// ---------------------------------------------------------------------------
struct FengyanRecentLayout {
  Rect panel{};
  Rect hero{};
  Rect heroCover{};
  Rect heroInfo{};
  Rect progress{};
  Rect mini[3]{};
  Rect miniCover[3]{};
  int dividerY = 0;
  int bookCount = 0;

  bool valid() const {
    return panel.width > 0 && panel.height > 0 && hero.width > 0 &&
           hero.height > 0 && bookCount > 0;
  }

  Rect bookRect(int index) const {
    if (index == 0) return hero;
    if (index >= 1 && index <= 3) return mini[index - 1];
    return {};
  }
};

inline FengyanRecentLayout makeFengyanRecentLayout(const Rect& rect, int bookCount,
                                                    int contentSidePadding = 20) {
  (void)contentSidePadding;
  FengyanRecentLayout L;
  L.bookCount = minInt(maxInt(bookCount, 0), 4);
  if (rect.width <= 0 || rect.height <= 0) return L;

  const int dy = rect.y - HomeRef::Recent.y;
  const int dx = rect.x;
  L.panel = {dx + HomeRef::Recent.x, HomeRef::Recent.y + dy, HomeRef::Recent.w, HomeRef::Recent.h};
  L.heroCover = {dx + HomeRef::HeroCover.x, HomeRef::HeroCover.y + dy, HomeRef::HeroCover.w, HomeRef::HeroCover.h};
  L.hero = {L.heroCover.x, L.heroCover.y, HomeRef::HeroTextRight - HomeRef::HeroCover.x, HomeRef::HeroCover.h};
  L.heroInfo = {dx + HomeRef::HeroTextX, HomeRef::HeroTitleBaseline + dy - HomeRef::FontHeroTitle,
                maxInt(1, HomeRef::HeroTextRight - HomeRef::HeroTextX),
                HomeRef::HeroProgressBar.y - HomeRef::HeroTitleBaseline + HomeRef::FontHeroTitle};
  L.progress = {dx + HomeRef::HeroProgressBar.x, HomeRef::HeroProgressBar.y + dy, HomeRef::HeroProgressBar.w,
                HomeRef::HeroProgressBar.h};
  L.dividerY = HomeRef::HeroDividerY + dy;
  L.miniCover[0] = {dx + HomeRef::MiniCover1.x, HomeRef::MiniCover1.y + dy, HomeRef::MiniCover1.w,
                    HomeRef::MiniCover1.h};
  L.miniCover[1] = {dx + HomeRef::MiniCover2.x, HomeRef::MiniCover2.y + dy, HomeRef::MiniCover2.w,
                    HomeRef::MiniCover2.h};
  L.miniCover[2] = {dx + HomeRef::MiniCover3.x, HomeRef::MiniCover3.y + dy, HomeRef::MiniCover3.w,
                    HomeRef::MiniCover3.h};
  const int miniTitleH = 24;
  for (int i = 0; i < 3; ++i) {
    L.mini[i] = {L.miniCover[i].x, L.miniCover[i].y, L.miniCover[i].width,
                 HomeRef::MiniTitleBaseline + dy - L.miniCover[i].y + miniTitleH};
  }
  if (L.bookCount <= 0) return L;
  return L;
}

inline bool fengyanRecentBookIndexFromPoint(const Rect& rect, int bookCount,
                                            int contentSidePadding, int px, int py,
                                            int& outIndex) {
  const auto layout = makeFengyanRecentLayout(rect, bookCount, contentSidePadding);
  if (!layout.valid() || !layout.panel.contains(px, py)) return false;
  const int n = layout.bookCount;
  for (int i = 0; i < n; ++i) {
    if (layout.bookRect(i).contains(px, py)) {
      outIndex = i;
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Fengyan home menu grid (shared by draw + hit-test)
// ---------------------------------------------------------------------------
// Layout: 3 columns, rows = ceil(buttonCount / 3), supports 1..N items (tested 4..8).
// offsetY matches FengyanTheme::drawButtonMenu.
struct FengyanMenuLayout {
  int cols = 3;
  int rows = 0;
  int horizontalSpacing = 16;
  int verticalSpacing = 12;
  int contentSidePadding = 20;
  int offsetY = -6;
  int squareSize = 0;
  int tileW = 0;
  int tileH = 0;
  int startX = 0;
  int startY = 0;
  int buttonCount = 0;
  int tileX[4] = {0, 0, 0, 0};
  bool useHomeRef = false;
  Rect adjusted{};

  bool valid() const { return (squareSize > 0 || tileW > 0) && buttonCount > 0 && rows > 0; }

  Rect tileRect(int index) const {
    if (index < 0 || index >= buttonCount || !valid()) return {};
    if (useHomeRef && index < 4) {
      return {tileX[index], startY, tileW, tileH};
    }
    const int col = index % cols;
    const int row = index / cols;
    return {startX + col * (squareSize + horizontalSpacing),
            startY + row * (squareSize + verticalSpacing), squareSize, squareSize};
  }
};

inline FengyanMenuLayout makeFengyanMenuLayout(const Rect& rect, int buttonCount, int contentSidePadding = 20,
                                               int offsetY = -6, int cols = 3) {
  FengyanMenuLayout L;
  L.cols = cols > 0 ? cols : 3;
  L.contentSidePadding = contentSidePadding;
  L.offsetY = offsetY;
  L.buttonCount = maxInt(0, buttonCount);
  L.adjusted = {rect.x, rect.y, rect.width, rect.height};
  if (L.buttonCount <= 0 || L.adjusted.width <= 0 || L.adjusted.height <= 0) return L;

  if (L.cols == 4) {
    const int dy = rect.y - HomeRef::Quick.y;
    const int n = minInt(L.buttonCount, 4);
    L.useHomeRef = true;
    L.rows = 1;
    L.tileW = HomeRef::QuickTile1.w;
    L.tileH = HomeRef::QuickTile1.h;
    L.squareSize = HomeRef::QuickTile1.w;
    L.startY = HomeRef::QuickTile1.y + dy;
    const HomeRef::Rect tiles[4] = {HomeRef::QuickTile1, HomeRef::QuickTile2, HomeRef::QuickTile3,
                                    HomeRef::QuickTile4};
    for (int i = 0; i < 4; ++i) L.tileX[i] = rect.x + tiles[i].x;
    L.buttonCount = n;
    L.startX = L.tileX[0];
    return L;
  }

  L.adjusted = {rect.x, rect.y + offsetY, rect.width, rect.height};
  L.rows = maxInt(1, ceilDiv(L.buttonCount, L.cols));
  const int totalHSpacing = L.horizontalSpacing * (L.cols - 1);
  const int totalVSpacing = L.verticalSpacing * (L.rows - 1);
  const int availableWidth = L.adjusted.width - contentSidePadding * 2 - totalHSpacing;
  const int availableHeight = L.adjusted.height - totalVSpacing;
  if (availableWidth <= 0 || availableHeight <= 0) return L;
  L.squareSize = minInt(availableWidth / L.cols, availableHeight / L.rows);
  if (L.squareSize <= 0) return L;
  const int totalGridWidth = L.squareSize * L.cols + totalHSpacing;
  L.startX = L.adjusted.x + (L.adjusted.width - totalGridWidth) / 2;
  L.startY = L.adjusted.y;
  return L;
}

inline bool fengyanMenuIndexFromPoint(const Rect& rect, int buttonCount, int px, int py, int& outIndex,
                                      int offsetY = -6, int contentSidePadding = 20, int cols = 3) {
  const auto layout = makeFengyanMenuLayout(rect, buttonCount, contentSidePadding, offsetY, cols);
  if (!layout.valid() || !layout.adjusted.contains(px, py)) return false;
  for (int i = 0; i < layout.buttonCount; ++i) {
    if (layout.tileRect(i).contains(px, py)) {
      outIndex = i;
      return true;
    }
  }
  return false;
}

// Fixed Home footer: history / apps / settings.
inline bool fengyanHomeBottomIndexFromPoint(int px, int py, int& outIndex,
                                            int screenWidth = HomeRef::ScreenW,
                                            int screenHeight = HomeRef::ScreenH) {
  const auto nav = makeBottomNavigationLayout(screenWidth, screenHeight, HomeRef::BottomH);
  if (!nav.valid()) return false;
  if (nav.back.contains(px, py)) { outIndex = 0; return true; }
  if (nav.home.contains(px, py)) { outIndex = 1; return true; }
  if (nav.menu.contains(px, py)) { outIndex = 2; return true; }
  return false;
}

// Lyra home menu: two columns with fixed-height rows, matching
// LyraTheme::drawButtonMenu.
struct LyraMenuLayout {
  int columns = 2;
  int rowHeight = 64;
  int spacing = 8;
  int contentSidePadding = 20;
  int buttonCount = 0;
  int tileWidth = 0;
  Rect adjusted{};

  bool valid() const { return tileWidth > 0 && buttonCount > 0; }

  Rect tileRect(int index) const {
    if (index < 0 || index >= buttonCount || !valid()) return {};
    return {adjusted.x + contentSidePadding + (spacing + tileWidth) * (index % columns),
            adjusted.y + (rowHeight + spacing) * (index / columns), tileWidth, rowHeight};
  }
};

inline LyraMenuLayout makeLyraMenuLayout(const Rect& rect, int buttonCount, int contentSidePadding = 20,
                                         int rowHeight = 64, int spacing = 8) {
  LyraMenuLayout L;
  L.contentSidePadding = contentSidePadding;
  L.rowHeight = rowHeight;
  L.spacing = spacing;
  L.buttonCount = maxInt(0, buttonCount);
  L.adjusted = rect;
  if (L.buttonCount <= 0 || rect.width <= 2 * contentSidePadding || rowHeight <= 0) return L;
  L.tileWidth = (rect.width - 2 * contentSidePadding - spacing) / 2;
  return L;
}

inline bool lyraMenuIndexFromPoint(const Rect& rect, int buttonCount, int px, int py, int& outIndex,
                                   int contentSidePadding = 20, int rowHeight = 64, int spacing = 8) {
  const auto layout = makeLyraMenuLayout(rect, buttonCount, contentSidePadding, rowHeight, spacing);
  if (!layout.valid() || !rect.contains(px, py)) return false;
  for (int i = 0; i < layout.buttonCount; ++i) {
    if (layout.tileRect(i).contains(px, py)) {
      outIndex = i;
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Fengyan recent covers (uses rect.x origin)
// ---------------------------------------------------------------------------
inline bool fengyanCoverIndexFromPoint(const Rect& rect, int coverCount, int contentSidePadding, int homeCoverHeight,
                                       int hPaddingInSelection, int px, int py, int& outIndex) {
  if (coverCount <= 0) return false;
  const int n = minInt(coverCount, 3);
  const int tileWidth = (rect.width - 2 * contentSidePadding) / 3;
  const int tileHeight = homeCoverHeight + hPaddingInSelection * 2;
  const int tileY = rect.y;
  const int coverSpacing = 8;
  const int totalCoversWidth = tileWidth * 3;
  const int availableCoverWidth = (totalCoversWidth - coverSpacing * 2) / 3;
  if (availableCoverWidth <= 0) return false;
  if (py < tileY || py >= tileY + tileHeight) return false;

  for (int i = 0; i < n; ++i) {
    const int tileX = rect.x + contentSidePadding + i * (availableCoverWidth + coverSpacing);
    if (px >= tileX && px < tileX + availableCoverWidth) {
      outIndex = i;
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Settings / list rows
// ---------------------------------------------------------------------------
// Fengyan/Lyra drawTabBar lays tabs left-to-right from contentSidePadding using
// each label's text width (+ horizontal pad) and tabSpacing — NOT equal thirds.
// Hit tests must use the same cumulative layout or the rightmost tab (System)
// is unreachable while empty right-side space is mis-assigned.
//
// tabWidths[i] = textWidth(label[i]) + 2*hPaddingInSelection (theme hPad is 8).
// outIndex is 0..tabCount-1. Hits expand to midpoints between adjacent tabs.
inline bool settingsTabFromPoint(int x, int y, int tabTop, int tabHeight, int contentSidePadding,
                                 int tabSpacing, int hPaddingInSelection, const int* tabTextWidths,
                                 int tabCount, int& outIndex) {
  if (tabCount <= 0 || tabTextWidths == nullptr || tabHeight <= 0) return false;
  if (y < tabTop || y >= tabTop + tabHeight) return false;

  // Build [left, right) of each painted tab chip.
  int lefts[16];
  int rights[16];
  if (tabCount > 16) tabCount = 16;
  int cur = contentSidePadding;
  for (int i = 0; i < tabCount; ++i) {
    const int chipW = tabTextWidths[i] + 2 * hPaddingInSelection;
    lefts[i] = cur;
    rights[i] = cur + maxInt(1, chipW);
    cur = rights[i] + tabSpacing;
  }

  for (int i = 0; i < tabCount; ++i) {
    // Expand hit zone to midpoints so gaps between chips still work.
    const int hitL = (i == 0) ? lefts[0] - contentSidePadding : (rights[i - 1] + lefts[i]) / 2;
    const int hitR = (i == tabCount - 1) ? rights[i] + tabSpacing + 24 : (rights[i] + lefts[i + 1]) / 2;
    if (x >= hitL && x < hitR) {
      outIndex = i;
      return true;
    }
  }
  return false;
}

// Settings UI: drawList(settingsCount, selectedSettingIndex - 1). There is NO
// category-label row in the list; category changes only via tabs.
// Rendered row r (0..settingsCount-1) maps to selectedSettingIndex = r + 1.
inline bool settingsRowFromPoint(int y, int listTop, int listHeight, int rowStep, int settingsCount,
                                 int selectedSettingIndex, int& outSelectedSettingIndex) {
  if (settingsCount <= 0 || rowStep <= 0) return false;
  if (y < listTop || y >= listTop + listHeight) return false;
  const int pageItems = maxInt(1, listHeight / rowStep);
  // selectedSettingIndex 0 means "tabs focused"; list highlight uses -1 → page 0
  const int listSelected = selectedSettingIndex > 0 ? selectedSettingIndex - 1 : 0;
  const int pageStart = maxInt(0, listSelected / pageItems) * pageItems;
  const int row = (y - listTop) / rowStep;
  if (row < 0 || row >= pageItems) return false;
  const int listIndex = pageStart + row;
  if (listIndex >= settingsCount) return false;
  outSelectedSettingIndex = listIndex + 1;
  return true;
}

// Generic list: point -> absolute item index with paging from selectedIndex.
inline bool listIndexFromPoint(int y, int listTop, int listHeight, int rowStep, int itemCount, int selectedIndex,
                               int& outIndex) {
  if (itemCount <= 0 || rowStep <= 0) return false;
  if (y < listTop || y >= listTop + listHeight) return false;
  const int pageItems = maxInt(1, listHeight / rowStep);
  const int pageStart = maxInt(0, selectedIndex / pageItems) * pageItems;
  const int row = (y - listTop) / rowStep;
  const int tapped = pageStart + row;
  if (row < 0 || row >= pageItems || tapped >= itemCount) return false;
  outIndex = tapped;
  return true;
}

// Centered vertical popup menu (MyLibrary action menu geometry).
struct PopupMenuLayout {
  int popupX = 0;
  int popupY = 0;
  int popupW = 0;
  int popupH = 0;
  int itemH = 0;
  int padding = 0;
  int itemCount = 0;

  Rect itemRect(int index) const {
    if (index < 0 || index >= itemCount) return {};
    return {popupX + 2, popupY + padding + index * itemH, popupW - 4, itemH};
  }
};

inline PopupMenuLayout makeCenteredPopupMenu(int screenW, int screenH, int itemCount, int popupW = 170,
                                             int itemH = 40, int padding = 4) {
  PopupMenuLayout L;
  L.itemCount = maxInt(0, itemCount);
  L.popupW = popupW;
  L.itemH = itemH;
  L.padding = padding;
  L.popupH = L.itemCount * itemH + padding * 2;
  L.popupX = (screenW - popupW) / 2;
  L.popupY = (screenH - L.popupH) / 2;
  return L;
}

inline bool popupMenuIndexFromPoint(const PopupMenuLayout& layout, int px, int py, int& outIndex) {
  if (layout.itemCount <= 0) return false;
  for (int i = 0; i < layout.itemCount; ++i) {
    if (layout.itemRect(i).contains(px, py)) {
      outIndex = i;
      return true;
    }
  }
  return false;
}

// Panel-normalized touch -> logical coordinates for a given orientation.
enum class Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };

inline void tapToLogical(float nx, float ny, int panelWidth, int panelHeight, Orientation orientation, int& outX,
                         int& outY) {
  int phyX = static_cast<int>(nx * panelWidth);
  int phyY = static_cast<int>(ny * panelHeight);
  phyX = maxInt(0, minInt(panelWidth - 1, phyX));
  phyY = maxInt(0, minInt(panelHeight - 1, phyY));
  switch (orientation) {
    case Orientation::Portrait:
      outX = panelHeight - 1 - phyY;
      outY = phyX;
      break;
    case Orientation::PortraitInverted:
      outX = phyY;
      outY = panelWidth - 1 - phyX;
      break;
    case Orientation::LandscapeClockwise:
      outX = panelWidth - 1 - phyX;
      outY = panelHeight - 1 - phyY;
      break;
    case Orientation::LandscapeCounterClockwise:
    default:
      outX = phyX;
      outY = phyY;
      break;
  }
}

enum class ReaderZone { None, Prev, Next, Menu };

inline ReaderZone readerZoneFromPoint(int x, int y, int screenW, int screenH) {
  if (screenW <= 0 || screenH <= 0) return ReaderZone::None;
  if (x < 0 || y < 0 || x >= screenW || y >= screenH) return ReaderZone::None;
  const int third = screenW / 3;
  if (x < third) return ReaderZone::Prev;
  if (x >= screenW - third) return ReaderZone::Next;
  return ReaderZone::Menu;
}

// Mandatory font IDs that M4 setup must register (host-checkable mirror of fontIds.h).
// Values must match src/fontIds.h.
constexpr int kMandatoryFontIds[] = {
    -1559651934,  // NOTOSANS_12
    -1014561631,  // NOTOSANS_14
    -1422711852,  // NOTOSANS_16
    1237754772,   // NOTOSANS_18
    -1246724383,  // UI_10
    -359249323,   // UI_12
    1073217904,   // SMALL
};
constexpr int kMandatoryFontIdCount = 7;

inline bool isMandatoryFontId(int id) {
  for (int i = 0; i < kMandatoryFontIdCount; ++i) {
    if (kMandatoryFontIds[i] == id) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Keyboard grid (KeyboardEntryActivity) — pure geometry from layout params
// ---------------------------------------------------------------------------
// Row 0: variable-width control buttons (QR/OK/Backspace/Shift/Space).
// Rows 1..N-2: fixed keyWidth cells. Matches KeyboardEntryActivity::render.
struct KeyboardKey {
  int row = 0;
  int col = 0;
  Rect rect{};
};

struct KeyboardLayout {
  int keyHeight = 26;
  int keySpacing = 5;
  int keyWidth = 0;
  int leftMargin = 0;
  int keyboardStartY = 0;
  int row0Count = 5;
  Rect row0[5]{};
  int charRows = 4;  // rows 1..4
  int keysPerCharRow = 13;
  // max keys stored for host tests; production walks getRowLength
  static constexpr int kMaxCharKeys = 13 * 4;
  KeyboardKey charKeys[kMaxCharKeys]{};
  int charKeyCount = 0;

  bool hit(int px, int py, int& outRow, int& outCol) const {
    for (int i = 0; i < row0Count; ++i) {
      if (row0[i].contains(px, py)) {
        outRow = 0;
        outCol = i;
        return true;
      }
    }
    for (int i = 0; i < charKeyCount; ++i) {
      if (charKeys[i].rect.contains(px, py)) {
        outRow = charKeys[i].row;
        outCol = charKeys[i].col;
        return true;
      }
    }
    return false;
  }
};

// row0Widths[5]: measured button widths; space uses remaining to maxRowWidth.
// charRowLengths[4]: keys in rows 1..4.
inline KeyboardLayout makeKeyboardLayout(int pageWidth, int keyboardStartY, int keyHeight, int keySpacing,
                                         int keysPerRow, const int row0Widths[5], const int charRowLengths[4]) {
  KeyboardLayout L;
  L.keyHeight = keyHeight;
  L.keySpacing = keySpacing;
  L.keyWidth = (pageWidth / keysPerRow) - keySpacing;
  const int maxRowWidth = keysPerRow * (L.keyWidth + keySpacing);
  L.leftMargin = (pageWidth - maxRowWidth) / 2;
  L.keyboardStartY = keyboardStartY;
  L.keysPerCharRow = keysPerRow;
  int x = L.leftMargin;
  const int rowY0 = keyboardStartY;
  for (int i = 0; i < 4; ++i) {
    L.row0[i] = {x, rowY0, row0Widths[i], keyHeight};
    x += row0Widths[i] + keySpacing;
  }
  // Space fills remainder of max row
  const int spaceW = maxInt(row0Widths[4], L.leftMargin + maxRowWidth - x);
  L.row0[4] = {x, rowY0, spaceW, keyHeight};
  L.row0Count = 5;
  L.charKeyCount = 0;
  for (int row = 1; row <= 4; ++row) {
    const int n = charRowLengths[row - 1];
    const int rowY = keyboardStartY + row * (keyHeight + keySpacing);
    for (int col = 0; col < n && L.charKeyCount < KeyboardLayout::kMaxCharKeys; ++col) {
      KeyboardKey k;
      k.row = row;
      k.col = col;
      k.rect = {L.leftMargin + col * (L.keyWidth + keySpacing), rowY, L.keyWidth, keyHeight};
      L.charKeys[L.charKeyCount++] = k;
    }
  }
  return L;
}

// Full-screen message / status: center band tap or any full-screen dismiss.
inline bool fullScreenDismissFromPoint(int px, int py, int screenW, int screenH) {
  return px >= 0 && py >= 0 && px < screenW && py < screenH;
}

// ---------------------------------------------------------------------------
// System navigation gestures (phone-like full-screen)
// ---------------------------------------------------------------------------
// Navigation gestures are edge-originating gestures, not generic swipes:
// Back accepts either edge: left-to-right or right-to-left. Home starts at the
// bottom edge and moves up. Narrow activation bands keep ordinary reader page
// turns out of system navigation.
constexpr float kBackEdgeFracX = 0.12f;  // rightmost 12% of screen
constexpr float kHomeEdgeFracY = 0.08f;  // bottom 8% of screen
constexpr int kNavGestureMinPx = 56;
constexpr int kNavGestureAxisRatio = 4;  // primary axis must be >= 4x cross-axis

inline int absInt(int v) { return v < 0 ? -v : v; }

inline bool isSystemBackSwipe(int sx, int sy, int ex, int ey, int screenW, int screenH) {
  (void)sy;
  (void)screenH;
  if (screenW <= 0) return false;
  const int dx = ex - sx;
  const int dy = ey - sy;
  if (absInt(dx) < kNavGestureMinPx) return false;
  if (absInt(dx) < absInt(dy) * kNavGestureAxisRatio) return false;
  const int edge = static_cast<int>(screenW * kBackEdgeFracX);
  return (sx <= edge && dx > 0) || (sx >= screenW - edge && dx < 0);
}

inline bool isSystemHomeSwipe(int sx, int sy, int ex, int ey, int screenW, int screenH) {
  (void)sx;
  (void)screenW;
  if (screenH <= 0) return false;
  const int dx = ex - sx;
  const int dy = ey - sy;  // up => negative dy in screen coords (y grows downward)
  if (dy >= -kNavGestureMinPx) return false;
  if (absInt(dy) < absInt(dx) * kNavGestureAxisRatio) return false;
  return sy >= screenH - static_cast<int>(screenH * kHomeEdgeFracY);
}

}  // namespace TouchHitGeometry
