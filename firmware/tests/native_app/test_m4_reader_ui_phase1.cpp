// M4 UI Redesign Phase 1 — Reader UI contracts (spec §7 / plan Task R).
// Build (Reader worktree root):
//   /opt/homebrew/bin/g++-14 -std=c++17 -I firmware/src \
//     firmware/tests/native_app/test_m4_reader_ui_phase1.cpp \
//     -o /tmp/test_m4_reader_ui_phase1 && /tmp/test_m4_reader_ui_phase1
// Catalog header is present. Follow-up RED is a named assert / missing
// identifier (Enabled/Dir reachable, full-font picker), not a toolchain miss.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#if __has_include("activities/reader/M4ReaderSettingsCatalog.h")
#include "activities/reader/M4ReaderSettingsCatalog.h"
#define HAS_READER_CATALOG 1
#else
#define HAS_READER_CATALOG 0
#endif

namespace {

std::string readFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string loadFirst(const char* const* candidates, int n) {
  for (int i = 0; i < n; ++i) {
    std::string c = readFile(candidates[i]);
    if (!c.empty()) {
      printf("loaded %s (%zu bytes)\n", candidates[i], c.size());
      return c;
    }
  }
  return {};
}

std::string loadMenuH() {
  const char* c[] = {
      "firmware/src/activities/reader/EpubReaderMenuActivity.h",
      "./firmware/src/activities/reader/EpubReaderMenuActivity.h",
      "../firmware/src/activities/reader/EpubReaderMenuActivity.h",
  };
  return loadFirst(c, 3);
}

std::string loadMenuCpp() {
  const char* c[] = {
      "firmware/src/activities/reader/EpubReaderMenuActivity.cpp",
      "./firmware/src/activities/reader/EpubReaderMenuActivity.cpp",
      "../firmware/src/activities/reader/EpubReaderMenuActivity.cpp",
  };
  return loadFirst(c, 3);
}

std::string loadSettingsCpp() {
  const char* c[] = {
      "firmware/src/activities/reader/EpubReaderSettingsActivity.cpp",
      "./firmware/src/activities/reader/EpubReaderSettingsActivity.cpp",
      "../firmware/src/activities/reader/EpubReaderSettingsActivity.cpp",
  };
  return loadFirst(c, 3);
}

std::string loadSettingsH() {
  const char* c[] = {
      "firmware/src/activities/reader/EpubReaderSettingsActivity.h",
      "./firmware/src/activities/reader/EpubReaderSettingsActivity.h",
      "../firmware/src/activities/reader/EpubReaderSettingsActivity.h",
  };
  return loadFirst(c, 3);
}

std::string extractFn(const std::string& src, const char* sig) {
  auto pos = src.find(sig);
  if (pos == std::string::npos) return {};
  auto brace = src.find('{', pos);
  if (brace == std::string::npos) return {};
  int depth = 0;
  for (size_t i = brace; i < src.size(); ++i) {
    if (src[i] == '{') ++depth;
    else if (src[i] == '}') {
      --depth;
      if (depth == 0) return src.substr(brace, i - brace + 1);
    }
  }
  return {};
}

bool contains(const std::string& s, const char* n) { return s.find(n) != std::string::npos; }

bool streq(const char* a, const char* b) {
  if (!a || !b) return a == b;
  return std::strcmp(a, b) == 0;
}

}  // namespace

#if !HAS_READER_CATALOG

int main() {
  printf("RED: activities/reader/M4ReaderSettingsCatalog.h not yet present\n");
  fflush(stdout);
  assert(false && "RED: Reader settings catalog header not yet present");
  return 1;
}

#else

namespace {

void testQuickPanelIsFourTabs() {
  assert(kM4ReaderQuickTabCount == 4);
  assert(streq(m4ReaderQuickTabLabel(0), "目录"));
  assert(streq(m4ReaderQuickTabLabel(1), "进度"));
  assert(streq(m4ReaderQuickTabLabel(2), "排版"));
  assert(streq(m4ReaderQuickTabLabel(3), "工具"));
  assert(streq(m4ReaderQuickTabLabel(-1), ""));
  assert(streq(m4ReaderQuickTabLabel(4), ""));
  printf("quick panel 目录/进度/排版/工具 PASS\n");
}

void testLayoutSubset() {
  assert(streq(m4ReaderLayoutSubsetLabel(0), "字体"));
  assert(streq(m4ReaderLayoutSubsetLabel(1), "字号"));
  assert(streq(m4ReaderLayoutSubsetLabel(2), "紧凑"));
  assert(streq(m4ReaderLayoutSubsetLabel(3), "标准"));
  assert(streq(m4ReaderLayoutSubsetLabel(4), "宽松"));
  printf("排版 subset 字体/字号/紧凑-标准-宽松 PASS\n");
}

void testAllBooksScopeAndLabels() {
  assert(streq(kM4ReaderSettingsTitle, "阅读设置"));
  assert(streq(kM4ReaderSettingsScopeCopy, "全部书籍"));
  assert(streq(kM4ReaderSettingsScopeKey, "readerScopeAllBooks"));
  assert(streq(kM4ReaderBodySizeLabel, "字号"));
  assert(streq(kM4ReaderUiFontLabel, "界面文字"));
  assert(streq(kM4ReaderShowImagesLabel, "显示插图"));
  assert(streq(m4ReaderSettingsPageLabelForKey("readerPixelSize"), "字号"));
  assert(streq(m4ReaderSettingsPageLabelForKey("uiFontSize"), "界面文字"));
  assert(streq(m4ReaderSettingsPageLabelForKey("showEpubImages"), "显示插图"));
  assert(m4ReaderSettingsPageLabelForKey("readerPixelSize") !=
         m4ReaderSettingsPageLabelForKey("uiFontSize"));
  printf("全部书籍 + 字号≠界面文字 + 显示插图 PASS\n");
}

void testPageIncludesAndExcludes() {
  assert(m4ReaderSettingsPageIncludesKey("readerScopeAllBooks"));
  assert(m4ReaderSettingsPageIncludesKey("readerPixelSize"));
  assert(m4ReaderSettingsPageIncludesKey("showEpubImages"));
  assert(m4ReaderSettingsPageIncludesKey("firstlineintented"));
  assert(m4ReaderSettingsPageIncludesKey("lineSpacing"));
  assert(m4ReaderSettingsPageIncludesKey("pageTurnAnimationEnabled"));
  assert(m4ReaderSettingsPageIncludesKey("pageTurnAnimationDir"));
  assert(!m4ReaderSettingsPageIncludesKey("uiFontSize"));
  assert(!m4ReaderSettingsPageIncludesKey("imageQuality"));
  assert(!m4ReaderSettingsPageIncludesKey("pageTurnAnimationSteps"));
  assert(!m4ReaderSettingsPageIncludesKey("pageTurnAnimationMult"));
  assert(!m4ReaderSettingsPageIncludesKey("pageTurnAnimationTp"));
  assert(!m4ReaderSettingsPageIncludesKey("pageTurnAnimationFrameRate"));
  assert(m4ReaderSettingsKeyIsSystemDisplay("uiFontSize"));
  assert(m4ReaderSettingsKeyIsSystemDisplay("imageQuality"));
  assert(!m4ReaderSettingsKeyIsSystemDisplay("showEpubImages"));
  assert(!m4ReaderSettingsKeyIsEngineering("pageTurnAnimationEnabled"));
  assert(!m4ReaderSettingsKeyIsEngineering("pageTurnAnimationDir"));
  assert(m4ReaderSettingsKeyIsEngineering("pageTurnAnimationSteps"));
  assert(m4ReaderSettingsKeyIsEngineering("pageTurnAnimationMult"));
  assert(m4ReaderSettingsKeyIsEngineering("pageTurnAnimationTp"));
  assert(m4ReaderSettingsKeyIsEngineering("pageTurnAnimationFrameRate"));
  assert(!m4ReaderSettingsKeyIsEngineering("readerPixelSize"));
  printf("reading page include/exclude PASS\n");
}

void testFullFontPickerReachable() {
  assert(streq(kM4ReaderFontPickerKey, "readerFontFamily"));
  assert(streq(kM4ReaderFontPickerLabel, "字体"));
  assert(m4ReaderSettingsPageIncludesKey(kM4ReaderFontPickerKey));
  assert(streq(m4ReaderSettingsPageLabelForKey(kM4ReaderFontPickerKey), "字体"));
  printf("full font picker catalog PASS\n");
}

void testSamplePreviewDefersReflow() {
  M4ReaderLayoutPreviewState s;
  m4ReaderLayoutSampleChanged(s);
  assert(s.samplePreviewDirty);
  assert(!s.bookReflowPending);
  m4ReaderLayoutSampleChanged(s);
  assert(s.samplePreviewDirty);
  assert(!s.bookReflowPending);
  m4ReaderLayoutPanelClosed(s);
  assert(!s.samplePreviewDirty);
  assert(s.bookReflowPending);
  m4ReaderLayoutConsumeReflow(s);
  assert(!s.bookReflowPending);
  M4ReaderLayoutPreviewState untouched;
  m4ReaderLayoutPanelClosed(untouched);
  assert(!untouched.bookReflowPending);
  printf("sample preview live / reflow on close PASS\n");
}

void testMenuSourceContracts() {
  const std::string menuH = loadMenuH();
  const std::string menuCpp = loadMenuCpp();
  assert(!menuH.empty());
  assert(!menuCpp.empty());

  assert(contains(menuH, "M4ReaderSettingsCatalog.h") || contains(menuCpp, "M4ReaderSettingsCatalog.h"));
  assert(contains(menuH, "\"目录\""));
  assert(contains(menuH, "\"进度\""));
  assert(contains(menuH, "\"排版\""));
  assert(contains(menuH, "\"工具\""));
  // Quick tabs must not still be the STYLE/MORE product copy 字体/更多.
  const auto quickItems = extractFn(menuH, "quickMenuItems");
  assert(!quickItems.empty());
  assert(contains(quickItems, "\"目录\""));
  assert(contains(quickItems, "\"进度\""));
  assert(contains(quickItems, "\"排版\""));
  assert(contains(quickItems, "\"工具\""));
  assert(!contains(quickItems, "\"字体\""));
  assert(!contains(quickItems, "\"更多\""));

  const auto moreRoot = extractFn(menuH, "moreRootItems");
  assert(!moreRoot.empty());
  assert(contains(moreRoot, "\"阅读设置\""));
  assert(contains(moreRoot, "\"书签管理\""));
  assert(!contains(moreRoot, "\"排版与字体\""));
  assert(!contains(moreRoot, "OPEN_MORE_TYPOGRAPHY"));
  assert(!contains(menuH, "typographyMenuItems"));
  assert(!contains(menuH, "MoreSection::TYPOGRAPHY"));

  // STYLE/MORE must not re-list the full reading catalog.
  assert(!contains(menuH, "\"行间距\""));
  assert(!contains(menuH, "\"上边距\""));
  assert(!contains(menuH, "\"对齐\""));
  assert(!contains(menuH, "\"显示插图\""));
  assert(!contains(menuH, "uiFontSize"));
  assert(!contains(menuH, "imageQuality"));
  assert(!contains(menuH, "pageTurnAnimationSteps"));
  assert(!contains(menuH, "pageTurnAnimationMult"));
  assert(!contains(menuH, "pageTurnAnimationTp"));

  const auto apply = extractFn(menuCpp, "EpubReaderMenuActivity::applyInternalAction");
  assert(!apply.empty());
  assert(contains(apply, "m4ReaderLayoutSampleChanged"));
  assert(!contains(apply, "notifyParentStyleChanged"));
  assert(!contains(apply, "onReaderMenuStyleChanged"));

  const auto loopFn = extractFn(menuCpp, "EpubReaderMenuActivity::loop");
  assert(!loopFn.empty());
  assert(contains(loopFn, "ty < panelTop + kStyleSheetHeaderH"));
  assert(contains(loopFn, "tx >= pageWidth - kTopBookmarkHitW"));
  assert(contains(loopFn, "closeToReader()"));

  const auto closeFn = extractFn(menuCpp, "EpubReaderMenuActivity::closeToReader");
  assert(!closeFn.empty());
  assert(contains(closeFn, "notifyParentStyleChanged"));

  const auto notify = extractFn(menuCpp, "EpubReaderMenuActivity::notifyParentStyleChanged");
  assert(!notify.empty());
  assert(contains(notify, "m4ReaderLayoutPanelClosed"));

  assert(!contains(menuH, "per-book"));
  assert(!contains(menuH, "perBook"));
  assert(!contains(menuCpp, "BookTypographyOverride"));
  printf("menu source contracts PASS\n");
}

void testSettingsSourceContracts() {
  const std::string settingsCpp = loadSettingsCpp();
  const std::string settingsH = loadSettingsH();
  assert(!settingsCpp.empty());
  assert(!settingsH.empty());
  assert(contains(settingsCpp, "M4ReaderSettingsCatalog.h") || contains(settingsH, "M4ReaderSettingsCatalog.h"));
  assert(contains(settingsCpp, "m4ReaderSettingsPageIncludesKey"));
  assert(contains(settingsCpp, "kM4ReaderSettingsScopeKey") || contains(settingsCpp, "readerScopeAllBooks"));
  assert(contains(settingsCpp, "全部书籍"));
  assert(contains(settingsCpp, "kM4ReaderSettingsTitle") || contains(settingsCpp, "\"阅读设置\""));
  assert(contains(settingsCpp, "m4ReaderSettingsPageLabelForKey"));
  assert(!contains(settingsCpp, "uiFontSize"));
  assert(!contains(settingsCpp, "imageQuality"));
  assert(!contains(settingsCpp, "pageTurnAnimationSteps"));
  assert(!contains(settingsCpp, "pageTurnAnimationMult"));
  assert(!contains(settingsCpp, "pageTurnAnimationTp"));
  assert(!contains(settingsCpp, "BookTypographyOverride"));
  assert(contains(settingsCpp, "FontSelectionActivity"));
  assert(contains(settingsCpp, "kM4ReaderFontPickerKey"));
  assert(contains(settingsCpp, "SettingInfo::Action"));
  const auto toggle = extractFn(settingsCpp, "EpubReaderSettingsActivity::toggleCurrentSetting");
  assert(!toggle.empty());
  assert(contains(toggle, "FontSelectionActivity"));
  assert(contains(toggle, "kM4ReaderFontPickerKey"));
  printf("settings source contracts PASS\n");
}

void testReadingV51CanvasGeometry() {
  // v5.1 detail pitch: shared paint/touch canvas runs 52px rows from y126;
  // G2 card top 516; selected stipple field 420x36 (v5.3: outline removed);
  // chevron tip at row+26.
  const std::string cpp = loadSettingsCpp();
  assert(!cpp.empty());
  assert(contains(cpp, "kReaderRowH = 52"));
  assert(contains(cpp, "kReaderWinTop = 126"));
  assert(contains(cpp, "516, 568, 620, 672") || contains(cpp, "516,568,620,672"));
  assert(contains(cpp, "fillRectStipple(30, y + 8, 420, 36)"));
  assert(contains(cpp, "y + 26"));
  printf("reading v5.1 canvas geometry PASS\n");
}

void testReadingV51BoundedThumb() {
  // v5.2 B: reader scroll thumb is a fixed ~30px weak indicator, never a
  // proportional long bar. Y stays on the existing pageStart/maxStart map;
  // paging, list height, touch, and navigation math are untouched.
  const std::string cpp = loadSettingsCpp();
  assert(!cpp.empty());
  assert(contains(cpp, "int thumbH = 30"));
  assert(!contains(cpp, "(trackH * pageItems) / count"));
  assert(!contains(cpp, "thumbH < 20"));
  assert(contains(cpp, "fillRect(470, thumbY, 1, thumbH, true)"));
  assert(contains(cpp, "((trackH - thumbH) * pageStart) / maxStart"));
  assert(contains(cpp, "count - pageItems"));
  printf("reading v5.1 bounded thumb PASS\n");
}

void testReadingV53SelectedStippleOnly() {
  // v5.3 B: Reading selected = stipple field only; the second full outline
  // is gone while row position/height, cards, and the 30px thumb stay put.
  const std::string cpp = loadSettingsCpp();
  assert(!cpp.empty());
  assert(!contains(cpp, "drawRoundedRect(26, y + 4, 428, 44, 1, 9, true)"));
  assert(contains(cpp, "fillRectStipple(30, y + 8, 420, 36)"));
  assert(contains(cpp, "int thumbH = 30"));
  assert(contains(cpp, "fillRect(470, thumbY, 1, thumbH, true)"));
  printf("reading v5.3 selected stipple-only PASS\n");
}

}  // namespace

int main() {
  testQuickPanelIsFourTabs();
  testLayoutSubset();
  testAllBooksScopeAndLabels();
  testPageIncludesAndExcludes();
  testFullFontPickerReachable();
  testSamplePreviewDefersReflow();
  testMenuSourceContracts();
  testSettingsSourceContracts();
  testReadingV51CanvasGeometry();
  testReadingV51BoundedThumb();
  testReadingV53SelectedStippleOnly();
  printf("reader UI phase1 tests passed\n");
  return 0;
}

#endif
