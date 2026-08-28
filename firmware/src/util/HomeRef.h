#pragma once

#include <cstdint>

namespace HomeRef {
struct Rect { int16_t x, y, w, h, r; };

constexpr int16_t ScreenW = 480;
constexpr int16_t ScreenH = 800;
constexpr int16_t PagePad = 16;
constexpr int16_t Stroke = 1;

// top status/header bar
constexpr int16_t HeaderY = 0;
constexpr int16_t HeaderH = 46;
constexpr int16_t HeaderPadX = 16;
constexpr int16_t HeaderIcon = 22;
constexpr int16_t HeaderDividerX = 47;
constexpr int16_t HeaderTitleX = 58;
constexpr int16_t HeaderTitleBaseline = 31;
constexpr int16_t HeaderTimeX = 314;
constexpr int16_t HeaderWifiX = 366;
constexpr int16_t HeaderBatteryTextX = 400;
constexpr int16_t HeaderBatteryX = 438;
constexpr int16_t HeaderBatteryW = 29;
constexpr int16_t HeaderBatteryH = 13;

// recent reading card
constexpr Rect Recent = {16, 62, 449, 481, 8};
constexpr int16_t RecentPadX = 18;             // inner left => 34
constexpr int16_t RecentTitleX = 35;
constexpr int16_t RecentTitleBaseline = 91;
constexpr int16_t RecentCountRight = 449;      // right-aligned, ~16 px outer margin
constexpr int16_t RecentCountBaseline = 91;

constexpr Rect HeroCover = {33, 105, 158, 222, 4};
constexpr int16_t HeroTextX = 208;
constexpr int16_t HeroTextRight = 449;
constexpr int16_t HeroTitleBaseline = 154;
constexpr int16_t HeroAuthorBaseline = 191;
constexpr int16_t HeroSourceBaseline = 226;
constexpr int16_t HeroProgressBaseline = 290;
constexpr int16_t HeroChapterBaseline = 290;
constexpr Rect HeroProgressBar = {208, 305, 234, 11, 0};
constexpr int16_t HeroDividerY = 341;
constexpr int16_t HeroDividerX1 = 33;
constexpr int16_t HeroDividerX2 = 442;

// two small recents underneath, intentionally lots of white space on the right
constexpr Rect MiniCover1 = {40, 363, 92, 127, 4};
constexpr Rect MiniCover2 = {161, 363, 92, 127, 4};
constexpr int16_t MiniTitleBaseline = 513;
constexpr int16_t MiniTitleCenter1 = 86;
constexpr int16_t MiniTitleCenter2 = 207;

// quick operations card
constexpr Rect Quick = {16, 557, 449, 172, 8};
constexpr int16_t QuickTitleX = 35;
constexpr int16_t QuickTitleBaseline = 589;
constexpr Rect QuickTile1 = {33, 603, 92, 107, 6};
constexpr Rect QuickTile2 = {141, 603, 92, 107, 6};
constexpr Rect QuickTile3 = {248, 603, 92, 107, 6};
constexpr Rect QuickTile4 = {357, 603, 92, 107, 6};
constexpr int16_t QuickIconY = 619;
constexpr int16_t QuickIconSize = 32;
constexpr int16_t QuickLabelBaseline = 692;

// fixed bottom nav
constexpr int16_t BottomY = 749;
constexpr int16_t BottomH = 51;
constexpr int16_t BottomSplit1 = 157;
constexpr int16_t BottomSplit2 = 318;
constexpr int16_t BottomIconSize = 22;
constexpr int16_t BottomBaseline = 782;

// typography: pixel heights / intended font sizes
constexpr int16_t FontHeader = 18;
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
