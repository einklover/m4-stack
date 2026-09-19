#pragma once

#include <cstdint>
#include <cstring>

// Phase 1 Reader UI catalog (spec §7). Host-testable; no Arduino/FreeRTOS.
// Typography is process-global (全部书籍). This header does not invent a
// per-book override store.

enum class M4ReaderQuickTab : uint8_t { Toc = 0, Progress = 1, Layout = 2, Tools = 3 };

constexpr int kM4ReaderQuickTabCount = 4;

inline const char* m4ReaderQuickTabLabel(int index) {
  static const char* kLabels[kM4ReaderQuickTabCount] = {"目录", "进度", "排版", "工具"};
  if (index < 0 || index >= kM4ReaderQuickTabCount) return "";
  return kLabels[index];
}

constexpr int kM4ReaderLayoutSubsetCount = 5;

inline const char* m4ReaderLayoutSubsetLabel(int index) {
  static const char* kLabels[kM4ReaderLayoutSubsetCount] = {"字体", "字号", "紧凑", "标准", "宽松"};
  if (index < 0 || index >= kM4ReaderLayoutSubsetCount) return "";
  return kLabels[index];
}

constexpr const char* kM4ReaderSettingsTitle = "阅读设置";
constexpr const char* kM4ReaderSettingsScopeCopy = "全部书籍";
constexpr const char* kM4ReaderSettingsScopeKey = "readerScopeAllBooks";
constexpr const char* kM4ReaderSettingsScopeTitle = "应用范围";
constexpr const char* kM4ReaderBodySizeLabel = "字号";
constexpr const char* kM4ReaderUiFontLabel = "界面文字";
constexpr const char* kM4ReaderShowImagesLabel = "显示插图";
constexpr const char* kM4ReaderFontPickerKey = "readerFontFamily";
constexpr const char* kM4ReaderFontPickerLabel = "字体";

inline bool m4ReaderSettingsKeyIsEngineering(const char* key) {
  if (!key) return false;
  // LUT / waveform knobs only. pageTurnAnimationEnabled and
  // pageTurnAnimationDir are user-facing Reader controls.
  return std::strcmp(key, "pageTurnAnimationSteps") == 0 ||
         std::strcmp(key, "pageTurnAnimationMult") == 0 ||
         std::strcmp(key, "pageTurnAnimationTp") == 0 ||
         std::strcmp(key, "pageTurnAnimationFrameRate") == 0;
}

inline bool m4ReaderSettingsKeyIsSystemDisplay(const char* key) {
  if (!key) return false;
  return std::strcmp(key, "uiFontSize") == 0 || std::strcmp(key, "imageQuality") == 0;
}

inline bool m4ReaderSettingsPageIncludesKey(const char* key) {
  if (!key || !key[0]) return false;
  if (std::strcmp(key, kM4ReaderSettingsScopeKey) == 0) return true;
  if (std::strcmp(key, kM4ReaderFontPickerKey) == 0) return true;
  if (m4ReaderSettingsKeyIsEngineering(key)) return false;
  if (m4ReaderSettingsKeyIsSystemDisplay(key)) return false;
  return true;
}

inline const char* m4ReaderSettingsPageLabelForKey(const char* key) {
  if (!key) return nullptr;
  if (std::strcmp(key, "readerPixelSize") == 0) return kM4ReaderBodySizeLabel;
  if (std::strcmp(key, "uiFontSize") == 0) return kM4ReaderUiFontLabel;
  if (std::strcmp(key, "showEpubImages") == 0) return kM4ReaderShowImagesLabel;
  if (std::strcmp(key, kM4ReaderSettingsScopeKey) == 0) return kM4ReaderSettingsScopeTitle;
  if (std::strcmp(key, kM4ReaderFontPickerKey) == 0) return kM4ReaderFontPickerLabel;
  return nullptr;
}

struct M4ReaderLayoutPreviewState {
  bool samplePreviewDirty = false;
  bool bookReflowPending = false;
};

inline void m4ReaderLayoutSampleChanged(M4ReaderLayoutPreviewState& state) {
  state.samplePreviewDirty = true;
}

inline void m4ReaderLayoutPanelClosed(M4ReaderLayoutPreviewState& state) {
  if (state.samplePreviewDirty) state.bookReflowPending = true;
  state.samplePreviewDirty = false;
}

inline void m4ReaderLayoutConsumeReflow(M4ReaderLayoutPreviewState& state) {
  state.bookReflowPending = false;
}
