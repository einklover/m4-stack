// Round 3 Lane C — SettingsActivity must have no tabs and no sdOta (string contract)
// Build:
//   /opt/homebrew/bin/g++-14 -std=c++17 -I firmware/src -I firmware/src/avr firmware/tests/native_app/test_settings_activity_no_tabs.cpp -o /tmp/test_settings_activity_no_tabs && /tmp/test_settings_activity_no_tabs
// Expected: RED until Luna removes drawTabBar and sdOta from SettingsActivity.cpp
// Do NOT edit SettingsActivity.cpp to make this green — it must be fixed by Luna lane.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>

namespace {

std::string readFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string loadSettingsActivity() {
  const char* candidates[] = {
    "firmware/src/activities/settings/SettingsActivity.cpp",
    "./firmware/src/activities/settings/SettingsActivity.cpp",
    "../../firmware/src/activities/settings/SettingsActivity.cpp",
    "/Volumes/z/paseo/workspaces/paseo/worktrees/041rfr5o/m4-home-muse-tests/firmware/src/activities/settings/SettingsActivity.cpp",
  };
  for (auto p : candidates) {
    std::string c = readFile(p);
    if (!c.empty()) {
      printf("loaded SettingsActivity.cpp from %s (%zu bytes)\n", p, c.size());
      return c;
    }
  }
  // Try absolute fallback via reading from current directory structure search
  // Last resort: try to locate via filesystem walk (not needed in CI)
  return {};
}

void testNoDrawTabBar(const std::string& src) {
  // Must not contain "drawTabBar" anywhere — old 3-tab paint path must be deleted.
  // Spec §8: Delete tab-focus, GUI.drawTabBar, categoryCount=3 paint path.
  bool has = src.find("drawTabBar") != std::string::npos;
  if (has) {
    // Show context
    size_t pos = src.find("drawTabBar");
    size_t start = pos > 80 ? pos - 80 : 0;
    size_t len = std::min<size_t>(200, src.size() - start);
    printf("RED: SettingsActivity.cpp still contains 'drawTabBar' at offset %zu\n", pos);
    printf("  context: ...%s...\n", src.substr(start, len).c_str());
    assert(!has && "SettingsActivity.cpp must NOT contain 'drawTabBar' — tab bar must be deleted per spec §8 (Hub↔L2 replaces 3-tab)");
  }
  printf("no drawTabBar PASS\n");
}

void testNoSdOta(const std::string& src) {
  // Must not contain "sdOta" (case-sensitive) — the string contract forbids reintroducing it.
  // Spec §4.4: Never sdOta / SdOtaUpdateActivity / SdMan.exists("/update/firmware.bin")
  bool hasSdOta = src.find("sdOta") != std::string::npos;
  bool hasSdOtaCap = src.find("SdOta") != std::string::npos; // catches class name
  bool hasUpdateFirmwareBin = src.find("/update/firmware.bin") != std::string::npos;
  if (hasSdOta || hasSdOtaCap) {
    size_t pos = src.find("sdOta");
    if (pos == std::string::npos) pos = src.find("SdOta");
    size_t start = pos > 80 ? pos - 80 : 0;
    size_t len = std::min<size_t>(200, src.size() - start);
    printf("RED: SettingsActivity.cpp still contains 'sdOta' or 'SdOta' at offset %zu\n", pos);
    printf("  context: ...%s...\n", src.substr(start, len).c_str());
    // Also report firmware.bin if present (old updater check)
    if (hasUpdateFirmwareBin) {
      size_t p2 = src.find("/update/firmware.bin");
      printf("  also contains '/update/firmware.bin' at %zu (forbidden SD updater check)\n", p2);
    }
    assert(!hasSdOta && !hasSdOtaCap && "SettingsActivity.cpp must NOT contain 'sdOta'/'SdOta' — forbidden per spec §4.4");
  }
  // Also fail if the string "/update/firmware.bin" remains (covers SdMan.exists check even if renamed)
  if (hasUpdateFirmwareBin) {
    printf("RED: SettingsActivity.cpp still contains '/update/firmware.bin' — forbidden SD updater path\n");
    assert(false && "must not contain '/update/firmware.bin'");
  }
  printf("no sdOta PASS\n");
}

void testNoLegacyCategoryTabStrings(const std::string& src) {
  // Soft check: old tab labels and categoryCount=3 should be gone.
  // This is secondary; the primary contracts are drawTabBar and sdOta.
  // We only warn if found, but still assert for categoryCount if present alongside drawTabBar.
  // To keep RED crisp, we just check that "categoryCount = 3" or "categoryCount=3" does not coexist with drawTabBar.
  // Here we enforce that if drawTabBar is already removed, this is just informational.
  bool hasCategory3 = src.find("categoryCount") != std::string::npos && src.find("3") != std::string::npos;
  bool hasSelectedZeroTabFocus = src.find("selectedSettingIndex==0") != std::string::npos ||
                                 src.find("selectedSettingIndex == 0") != std::string::npos;
  if (hasSelectedZeroTabFocus) {
    printf("WARN: SettingsActivity.cpp still contains 'selectedSettingIndex==0' tab-focus (should be deleted)\n");
    // This is not a hard fail per task (only drawTabBar/sdOta are contracted), but we note it.
  }
  (void)hasCategory3;
  printf("legacy tab strings check done (informational)\n");
}

} // namespace

int main() {
  std::string src = loadSettingsActivity();
  if (src.empty()) {
    printf("SKIP: cannot locate firmware/src/activities/settings/SettingsActivity.cpp from this cwd\n");
    printf("  tried relative paths from repo root and absolute worktree path\n");
    printf("  run from repo root: /Volumes/z/paseo/workspaces/paseo/worktrees/041rfr5o/m4-home-muse-tests\n");
    assert(false && "cannot locate SettingsActivity.cpp to scan — run from repo root");
    return 1;
  }
  testNoDrawTabBar(src);
  testNoSdOta(src);
  testNoLegacyCategoryTabStrings(src);
  printf("SettingsActivity no-tabs/no-sdOta ALL PASS\n");
  return 0;
}
