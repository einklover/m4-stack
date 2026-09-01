#pragma once

#include <cstdint>

namespace HomeRef {
struct Rect { int16_t x, y, w, h, r; };

constexpr int16_t ScreenW = 480;
constexpr int16_t ScreenH = 800;
constexpr int16_t PagePad = 18;
constexpr int16_t Stroke = 1;

// Measured from the 919x1536 reference and mapped to the 480x800 M4 panel.
constexpr int16_t HeaderY = 0;
constexpr int16_t HeaderH = 46;
// Non-Home Fengyan headers need physical top air; Home's generated reference
// scene owns its own 60px header and does not use this inset.
constexpr int16_t HeaderSafeTop = 20;
constexpr int16_t HeaderPadX = 27;
constexpr int16_t HeaderIcon = 22;
constexpr int16_t HeaderDividerX = 47;  // reference Home has no vertical header divider
constexpr int16_t HeaderTitleX = 58;
// Tune title ink against the centered status glyphs independently of SafeTop.
constexpr int16_t HeaderTitleBaseline = 38;
constexpr int16_t HeaderTimeX = 314;   // non-Home Fengyan compatibility
constexpr int16_t HeaderWifiX = 366;
constexpr int16_t HeaderBatteryTextX = 400;
constexpr int16_t HeaderBatteryX = 438;
constexpr int16_t HeaderBatteryW = 29;
constexpr int16_t HeaderBatteryH = 13;

// Home-only header geometry measured from the reference.
constexpr int16_t HomeHeaderH = 60;
constexpr int16_t HomeHeaderTitleX = 27;
constexpr int16_t HomeHeaderTitleBaseline = 43;
constexpr int16_t HomeHeaderWifiX = 430;
constexpr int16_t HomeHeaderIcon = 22;

// recent reading card
constexpr Rect Recent = {18, 77, 443, 543, 8};
constexpr int16_t RecentPadX = 19;
constexpr int16_t RecentTitleX = 37;
constexpr int16_t RecentTitleBaseline = 111;
constexpr int16_t RecentCountRight = 444;
constexpr int16_t RecentCountBaseline = 111;

constexpr Rect HeroCover = {25, 93, 164, 250, 4};
constexpr int16_t HeroTextX = 216;
constexpr int16_t HeroTextRight = 452;
constexpr int16_t HeroTitleBaseline = 198;
constexpr int16_t HeroAuthorBaseline = 238;
constexpr int16_t HeroSourceBaseline = 278;
constexpr int16_t HeroProgressBaseline = 331;
constexpr int16_t HeroChapterBaseline = 331;
constexpr Rect HeroProgressBar = {216, 316, 237, 26, 13};
constexpr int16_t HeroDividerY = 403;
constexpr int16_t HeroDividerX1 = 35;
constexpr int16_t HeroDividerX2 = 443;

// Three compact recent books, evenly balanced around x=98/240/379.
constexpr Rect MiniCover1 = {37, 416, 110, 146, 4};
constexpr Rect MiniCover2 = {185, 416, 106, 146, 4};
constexpr Rect MiniCover3 = {329, 416, 106, 146, 4};
constexpr int16_t MiniTitleBaseline = 589;
constexpr int16_t MiniTitleCenter1 = 92;
constexpr int16_t MiniTitleCenter2 = 238;
constexpr int16_t MiniTitleCenter3 = 382;

// application shortcuts card
constexpr Rect Quick = {18, 637, 443, 117, 8};
constexpr Rect QuickTile1 = {35, 650, 86, 92, 6};
constexpr Rect QuickTile2 = {141, 650, 86, 92, 6};
constexpr Rect QuickTile3 = {247, 650, 86, 92, 6};
constexpr Rect QuickTile4 = {357, 650, 86, 92, 6};
constexpr int16_t QuickIconY = 660;
constexpr int16_t QuickIconSize = 42;
constexpr int16_t QuickLabelBaseline = 739;

// fixed Home navigation
constexpr int16_t BottomY = 741;
constexpr int16_t BottomH = 45;
constexpr int16_t BottomSplit1 = 153;
constexpr int16_t BottomSplit2 = 304;
constexpr int16_t BottomIconSize = 22;
constexpr int16_t BottomBaseline = 773;

// typography: pixel heights / intended font sizes
constexpr int16_t FontHeader = 22;
constexpr int16_t FontSection = 20;
constexpr int16_t FontHeroTitle = 24;
constexpr int16_t FontHeroAuthor = 17;
constexpr int16_t FontMeta = 16;
constexpr int16_t FontProgress = 16;
constexpr int16_t FontMiniTitle = 16;
constexpr int16_t FontQuickLabel = 16;
constexpr int16_t FontBottom = 17;

// visual rules
constexpr int16_t CardRadius = 8;
constexpr int16_t TileRadius = 6;
constexpr int16_t CoverRadius = 4;
constexpr int16_t FocusInset = 3;
}  // namespace HomeRef
