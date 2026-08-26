#pragma once

#include <cstdint>

#include "util/TouchHitGeometry.h"

class GfxRenderer;
struct Rect;

// Shared touch-navigation policy for Murphy M4 chrome.
//
// Reader body / home: None (gesture + hardware only, no extra chrome)
// HeaderBack: compact explicit Back affordance in the standard title bar
// BottomBackHome: Android-like Back/Home bar replacing legacy button hints
//
// State is process-global because drawing happens on activity display tasks
// while system gesture routing happens on the main loop. The implementation
// stores it atomically so both paths see a coherent mode.
namespace M4TouchNavigation {

enum class Mode : uint8_t { None = 0, HeaderBack = 1, BottomBackHome = 2, ChapterHeaderBack = 3 };

void setMode(Mode mode);
Mode mode();
bool enabled();

// Standard activities start with HeaderBack. Screens that call
// GUI.drawButtonHints are automatically promoted to BottomBackHome by the
// UITheme facade, so old button-hint space becomes reliable touch navigation.
void activateForActivity(bool showNavigation);
void activateForChapterSelection();

bool hitBack(int x, int y, int screenWidth, int screenHeight);
bool hitHome(int x, int y, int screenWidth, int screenHeight);

// Draw only the affordance appropriate to the current mode.
void drawHeaderBack(const GfxRenderer& renderer, const Rect& headerRect, const char* title = nullptr);
void drawBottomBar(GfxRenderer& renderer);

constexpr int kHeaderHitWidth = 56;
constexpr int kHeaderHitHeight = 56;
constexpr int kBottomBarHeight = 50;
constexpr int kChapterHeaderHitWidth = TouchHitGeometry::kChapterHeaderBackWidth;
constexpr int kChapterHeaderHitHeight = TouchHitGeometry::kChapterHeaderBackHeight;

}  // namespace M4TouchNavigation
