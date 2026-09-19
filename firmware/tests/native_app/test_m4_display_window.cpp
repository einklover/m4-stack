// M4 UI Redesign Phase 1 — shared displayWindow choke-point
// Build (worktree root):
//   /opt/homebrew/bin/g++-14 -std=c++17 \
//     -I firmware/src -I firmware/lib/GfxRenderer \
//     firmware/tests/native_app/test_m4_display_window.cpp \
//     -o /tmp/test_m4_display_window && /tmp/test_m4_display_window
// RED: missing GfxDisplayWindow.h, or production still submitPartial→displayBuffer.

#include <cassert>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#if __has_include("GfxDisplayWindow.h")
#include "GfxDisplayWindow.h"
#define HAS_GFX_WINDOW 1
#else
#define HAS_GFX_WINDOW 0
#endif

namespace {

std::string loadFile(const char* path) {
  std::ifstream in(path);
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string loadFirst(const char* const* paths, int n) {
  for (int i = 0; i < n; ++i) {
    std::string s = loadFile(paths[i]);
    if (!s.empty()) return s;
  }
  return {};
}

std::string loadHalH() {
  const char* c[] = {"firmware/lib/hal/HalDisplay.h", "./firmware/lib/hal/HalDisplay.h"};
  return loadFirst(c, 2);
}
std::string loadHalCpp() {
  const char* c[] = {"firmware/lib/hal/HalDisplay.cpp", "./firmware/lib/hal/HalDisplay.cpp"};
  return loadFirst(c, 2);
}
std::string loadGfxH() {
  const char* c[] = {"firmware/lib/GfxRenderer/GfxRenderer.h", "./firmware/lib/GfxRenderer/GfxRenderer.h"};
  return loadFirst(c, 2);
}
std::string loadGfxCpp() {
  const char* c[] = {"firmware/lib/GfxRenderer/GfxRenderer.cpp", "./firmware/lib/GfxRenderer/GfxRenderer.cpp"};
  return loadFirst(c, 2);
}
std::string loadSettingsCpp() {
  const char* c[] = {"firmware/src/activities/settings/SettingsActivity.cpp",
                     "./firmware/src/activities/settings/SettingsActivity.cpp"};
  return loadFirst(c, 2);
}

std::string sliceFn(const std::string& src, const char* sig, const char* nextSig) {
  auto a = src.find(sig);
  if (a == std::string::npos) return {};
  auto b = src.find(nextSig, a + 1);
  if (b == std::string::npos) b = src.size();
  return src.substr(a, b - a);
}

void testHeaderPresent() {
#if !HAS_GFX_WINDOW
  assert(false && "RED: GfxDisplayWindow.h not yet present");
#else
  (void)0;
#endif
}

void testPortraitSettingsDirtyRect() {
#if HAS_GFX_WINDOW
  // Logical Settings old∪new: x=0 y=68 w=480 h=164 on 480x800 portrait.
  // Four corners map to panel AABB x=68 y=0 w=164 h=480, then x/w 8px snap.
  const GfxPanelRect r =
      gfxLogicalWindowToPanel(kGfxOrientPortrait, 0, 68, 480, 164, 800, 480);
  assert(r.valid);
  assert(r.x == 64);
  assert(r.y == 0);
  assert(r.w == 168);
  assert(r.h == 480);
  assert((r.x % 8) == 0);
  assert((r.w % 8) == 0);
  printf("portrait Settings dirty AABB+snap PASS\n");
#endif
}

void testEightPxSnapAndClamp() {
#if HAS_GFX_WINDOW
  GfxPanelRect a = gfxLogicalWindowToPanel(kGfxOrientLandscapeCCW, 1, 10, 1, 3, 800, 480);
  assert(a.valid);
  assert(a.x == 0);
  assert(a.y == 10);
  assert(a.w == 8);
  assert(a.h == 3);

  GfxPanelRect aligned = gfxLogicalWindowToPanel(kGfxOrientLandscapeCCW, 16, 20, 32, 8, 800, 480);
  assert(aligned.valid);
  assert(aligned.x == 16 && aligned.w == 32 && aligned.y == 20 && aligned.h == 8);

  GfxPanelRect full = gfxLogicalWindowToPanel(kGfxOrientPortrait, 0, 0, 480, 800, 800, 480);
  assert(full.valid);
  assert(full.x == 0 && full.y == 0 && full.w == 800 && full.h == 480);

  GfxPanelRect bad = gfxLogicalWindowToPanel(kGfxOrientPortrait, 0, 0, 0, 10, 800, 480);
  assert(!bad.valid);

  GfxPanelRect clip = gfxLogicalWindowToPanel(kGfxOrientLandscapeCCW, -4, -2, 20, 10, 800, 480);
  assert(clip.valid);
  assert(clip.x == 0);
  assert(clip.y == 0);
  assert((clip.x % 8) == 0 && (clip.w % 8) == 0);
  printf("8px snap + clamp PASS\n");
#endif
}

void testHalDeclaresAndForwardsWindow() {
  const std::string h = loadHalH();
  const std::string cpp = loadHalCpp();
  assert(!h.empty() && !cpp.empty());
  assert(h.find("void displayWindow(") != std::string::npos &&
         "HalDisplay.h must expose displayWindow(panel-native x,y,w,h)");
  const std::string fn = sliceFn(cpp, "void HalDisplay::displayWindow", "void HalDisplay::");
  assert(!fn.empty() && "RED: HalDisplay::displayWindow not implemented");
  assert(fn.find("einkDisplay.displayWindow") != std::string::npos &&
         "device path must forward FreeInkDisplay::displayWindow");
  assert(fn.find("waveformLab") == std::string::npos && "do not reuse Reader waveformLab window");
  // QEMU/plugin-debug may dump the full frame, but only as an explicit simulator fallback.
  if (fn.find("displayBuffer") != std::string::npos || fn.find("dumpQemuFrame") != std::string::npos) {
    assert(fn.find("Simulator fallback") != std::string::npos &&
           "QEMU full dump must be labeled Simulator fallback, not silent full FAST");
  }
  printf("HalDisplay displayWindow forward PASS\n");
}

void testGfxMapsThenCallsHalWindow() {
  const std::string h = loadGfxH();
  const std::string cpp = loadGfxCpp();
  assert(!h.empty() && !cpp.empty());
  assert(h.find("void displayWindow(") != std::string::npos);
  assert(h.find("// void displayWindow") == std::string::npos &&
         "GfxRenderer::displayWindow must be a live declaration, not a comment");
  const std::string fn = sliceFn(cpp, "void GfxRenderer::displayWindow", "void GfxRenderer::");
  assert(!fn.empty() && "RED: GfxRenderer::displayWindow not implemented");
  assert(fn.find("gfxLogicalWindowToPanel") != std::string::npos &&
         "Gfx must map logical rect → panel AABB+snap");
  assert(fn.find("display.displayWindow") != std::string::npos);
  assert(fn.find("displayBuffer") == std::string::npos &&
         "Gfx displayWindow must not internally full FAST");
  assert(fn.find("waveformLab") == std::string::npos);
  printf("GfxRenderer displayWindow mapping PASS\n");
}

void testSettingsPartialCallsWindowFullKeepsBuffer() {
  const std::string src = loadSettingsCpp();
  assert(!src.empty());
  const std::string fn = sliceFn(src, "void SettingsActivity::submitDisplay", "void SettingsActivity::");
  assert(!fn.empty());
  assert(fn.find("displayWindow") != std::string::npos &&
         "RED: submitPartial still ignores rect — production must call renderer.displayWindow");
  const auto firstBuf = fn.find("displayBuffer");
  assert(firstBuf != std::string::npos && "Full/first/burst-6 must still displayBuffer");
  const auto secondBuf = fn.find("displayBuffer", firstBuf + 1);
  assert(secondBuf == std::string::npos &&
         "Partial adapter must not also displayBuffer (that is the fake-green full FAST)");
  const auto win = fn.find("displayWindow");
  assert(win > firstBuf && "Full lambda is displayBuffer; Partial lambda is displayWindow");
  printf("Settings Partial→window Full→buffer PASS\n");
}

}  // namespace

int main() {
  testHeaderPresent();
  testPortraitSettingsDirtyRect();
  testEightPxSnapAndClamp();
  testHalDeclaresAndForwardsWindow();
  testGfxMapsThenCallsHalWindow();
  testSettingsPartialCallsWindowFullKeepsBuffer();
  printf("ALL displayWindow contracts PASS\n");
  return 0;
}
