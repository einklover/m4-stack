#!/usr/bin/env python3
from pathlib import Path


def replace_once(path: str, old: str, new: str, label: str) -> None:
    p = Path(path)
    s = p.read_text(encoding="utf-8")
    if new in s:
        return
    if old not in s:
        raise SystemExit(f"{label}: anchor not found in {path}")
    p.write_text(s.replace(old, new, 1), encoding="utf-8")


# 1) Plugin XML is the only source of entry-screen hierarchy.
replace_once(
    "firmware/src/activities/apps/NativeAppActivity.cpp",
    '#include "apps/native/M4NativeAppControllerFactory.h"\n#include "apps/native/M4NativeProviderHomeTemplate.h"\n',
    '#include "apps/native/M4NativeAppControllerFactory.h"\n',
    "remove home template include",
)

replace_once(
    "firmware/src/activities/apps/NativeAppActivity.cpp",
    '''bool NativeAppActivity::loadDocument() {
  M4NativeUi::Limits limits;
  if (const char* builtin = M4NativeProviderHomeTemplate::xmlFor(app_.provider)) {
    const size_t n = std::strlen(builtin);
    auto parsed = M4NativeUi::parse(builtin, n, limits);
    if (!parsed) {
      setError(std::string("ui_parse:") + M4NativeUi::errorKey(parsed.error));
      return false;
    }
    document_ = std::move(parsed.document);
    return true;
  }

  std::string path = app_.path;
''',
    '''bool NativeAppActivity::loadDocument() {
  M4NativeUi::Limits limits;
  std::string path = app_.path;
''',
    "remove provider home override",
)

# 2) Reset shared tile/list focus whenever entering a screen.
for label in ("enter focus reset", "navigate focus reset"):
    p = Path("firmware/src/activities/apps/NativeAppActivity.cpp")
    s = p.read_text(encoding="utf-8")
    old = '''  selectedIndex_ = 0;
  tabIndex_ = 0;
  resetFlowPaging();
'''
    new = '''  selectedIndex_ = 0;
  tabIndex_ = 0;
  tilesSelectedIndex_ = 0;
  tilesFocused_ = true;
  resetFlowPaging();
'''
    if new in s:
        break
    if old not in s:
        raise SystemExit(f"{label}: anchor not found")
    p.write_text(s.replace(old, new, 1), encoding="utf-8")

# The Navigate case contains the same reset block a second time.
p = Path("firmware/src/activities/apps/NativeAppActivity.cpp")
s = p.read_text(encoding="utf-8")
old = '''      selectedIndex_ = 0;
      tabIndex_ = 0;
      resetFlowPaging();
'''
new = '''      selectedIndex_ = 0;
      tabIndex_ = 0;
      tilesSelectedIndex_ = 0;
      tilesFocused_ = true;
      resetFlowPaging();
'''
if new not in s:
    if old not in s:
        raise SystemExit("navigate focus reset: anchor not found")
    p.write_text(s.replace(old, new, 1), encoding="utf-8")

# 3) Hardware focus: 4-column tile navigation, then list; Up from list top returns to tiles.
p = Path("firmware/src/activities/apps/NativeAppActivity.cpp")
s = p.read_text(encoding="utf-8")
if "if (tilesCount_ > 0 && tilesFocused_)" not in s:
    start_marker = '''  if (listCount_ > 0) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
'''
    end_marker = '''  if (mappedInput.wasReleased(MappedInputManager::Button::Left) && !buttonActions_[2].empty()) {
'''
    start = s.find(start_marker)
    end = s.find(end_marker, start)
    if start < 0 or end < 0:
        raise SystemExit("hardware focus input block: anchors not found")
    new_block = '''  if (tilesCount_ > 0 && tilesFocused_) {
    tilesSelectedIndex_ = std::max(0, std::min(tilesSelectedIndex_, tilesCount_ - 1));
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      if (tilesSelectedIndex_ >= tilesColumns_) tilesSelectedIndex_ -= tilesColumns_;
      updateRequired_ = true;
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      if (tilesSelectedIndex_ + tilesColumns_ < tilesCount_) {
        tilesSelectedIndex_ += tilesColumns_;
      } else if (listCount_ > 0) {
        tilesFocused_ = false;
        selectedIndex_ = 0;
      }
      updateRequired_ = true;
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      if (tilesSelectedIndex_ % tilesColumns_ > 0) --tilesSelectedIndex_;
      updateRequired_ = true;
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      const int col = tilesSelectedIndex_ % tilesColumns_;
      if (col + 1 < tilesColumns_ && tilesSelectedIndex_ + 1 < tilesCount_) ++tilesSelectedIndex_;
      updateRequired_ = true;
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (tilesNode_) handleAction(tilesNode_->action, tilesNode_, tilesSelectedIndex_);
      return;
    }
  }

  if (listCount_ > 0) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      if (selectedIndex_ == 0 && tilesCount_ > 0) {
        tilesFocused_ = true;
        tilesSelectedIndex_ = std::max(0, std::min(tilesSelectedIndex_, tilesCount_ - 1));
      } else {
        selectedIndex_ = (selectedIndex_ + listCount_ - 1) % listCount_;
      }
      updateRequired_ = true;
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      selectedIndex_ = (selectedIndex_ + 1) % listCount_;
      updateRequired_ = true;
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      handleAction(listAction_.empty() ? buttonActions_[1] : listAction_, nullptr, selectedIndex_);
      return;
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && !buttonActions_[1].empty()) {
    handleAction(buttonActions_[1]);
    return;
  }

'''
    s = s[:start] + new_block + s[end:]
    p.write_text(s, encoding="utf-8")

# Touch and keyboard mutate the same focus state.
replace_once(
    "firmware/src/activities/apps/NativeAppActivity.cpp",
    '''      if (col >= 0 && col < columns && row >= 0 && row < rows && index >= 0 &&
          index < tilesCount_ && inCellX < cellWidth && inCellY < cellHeight) {
        handleAction(tilesNode_->action, tilesNode_, index);
        return;
      }
''',
    '''      if (col >= 0 && col < columns && row >= 0 && row < rows && index >= 0 &&
          index < tilesCount_ && inCellX < cellWidth && inCellY < cellHeight) {
        tilesSelectedIndex_ = index;
        tilesFocused_ = true;
        updateRequired_ = true;
        handleAction(tilesNode_->action, tilesNode_, index);
        return;
      }
''',
    "tile touch focus",
)

replace_once(
    "firmware/src/activities/apps/NativeAppActivity.cpp",
    '''  if (TouchHitGeometry::listIndexFromPoint(ty, listTop_, listHeight_, rowHeight,
                                           listCount_, selectedIndex_, hit)) {
    selectedIndex_ = hit;
''',
    '''  if (TouchHitGeometry::listIndexFromPoint(ty, listTop_, listHeight_, rowHeight,
                                           listCount_, selectedIndex_, hit)) {
    tilesFocused_ = false;
    selectedIndex_ = hit;
''',
    "list touch focus",
)

# Clamp tile focus and render keyboard focus separately from the active category.
replace_once(
    "firmware/src/activities/apps/NativeAppActivity.cpp",
    '''        tilesCount_ = count;
        tilesColumns_ = columns;
        tilesNode_ = &node;
        for (int i = 0; i < count; ++i) {
''',
    '''        tilesCount_ = count;
        tilesColumns_ = columns;
        tilesNode_ = &node;
        if (count > 0) tilesSelectedIndex_ = std::max(0, std::min(tilesSelectedIndex_, count - 1));
        for (int i = 0; i < count; ++i) {
''',
    "tile focus clamp",
)

replace_once(
    "firmware/src/activities/apps/NativeAppActivity.cpp",
    '''          const bool selected = row.value == "selected";
          if (selected && cellWidth > 8 && cellHeight > 8) renderer.fillRoundedRect(x + 3, tileY + 3, cellWidth - 6, cellHeight - 6, 7, Color::LightGray);
          const std::string titleText = M4UiText::truncated(renderer, UI_10_FONT_ID, row.title.c_str(), std::max(1, cellWidth - 8), selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
          M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, x, tileY, cellWidth, cellHeight, titleText.c_str(), true, selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR, 4);
''',
    '''          const bool active = row.value == "selected";
          const bool focused = tilesFocused_ && i == tilesSelectedIndex_;
          if (focused && cellWidth > 8 && cellHeight > 8) {
            renderer.fillRoundedRect(x + 3, tileY + 3, cellWidth - 6, cellHeight - 6, 7, Color::LightGray);
          }
          const auto family = (active || focused) ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
          const std::string titleText = M4UiText::truncated(renderer, UI_10_FONT_ID, row.title.c_str(),
                                                            std::max(1, cellWidth - 8), family);
          M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, x, tileY, cellWidth, cellHeight,
                                     titleText.c_str(), true, family, 4);
''',
    "tile focus paint",
)

# Structured UI makes the key-focus journey testable without OCR.
replace_once(
    "firmware/src/activities/apps/NativeAppActivity.cpp",
    '''std::string NativeAppActivity::debugUiJson() {
  if (subActivity) return subActivity->debugUiJson();
  return "{\\\"kind\\\":\\\"native_app\\\",\\\"app_id\\\":\\\"" + jsonEscape(app_.id) +
         "\\\",\\\"provider\\\":\\\"" + jsonEscape(app_.provider) + "\\\",\\\"screen\\\":\\\"" +
         jsonEscape(screenId_) + "\\\",\\\"selected\\\":" + std::to_string(selectedIndex_) +
         ",\\\"rows\\\":" + std::to_string(listCount_) + ",\\\"error\\\":\\\"" + jsonEscape(error_) + "\\\"}";
}
''',
    '''std::string NativeAppActivity::debugUiJson() {
  if (subActivity) return subActivity->debugUiJson();
  const bool tileFocus = tilesCount_ > 0 && tilesFocused_;
  return "{\\\"kind\\\":\\\"native_app\\\",\\\"app_id\\\":\\\"" + jsonEscape(app_.id) +
         "\\\",\\\"provider\\\":\\\"" + jsonEscape(app_.provider) + "\\\",\\\"screen\\\":\\\"" +
         jsonEscape(screenId_) + "\\\",\\\"focus\\\":\\\"" + (tileFocus ? "tiles" : "list") +
         "\\\",\\\"tile_selected\\\":" + std::to_string(tilesSelectedIndex_) +
         ",\\\"tiles\\\":" + std::to_string(tilesCount_) +
         ",\\\"selected\\\":" + std::to_string(selectedIndex_) +
         ",\\\"rows\\\":" + std::to_string(listCount_) + ",\\\"error\\\":\\\"" + jsonEscape(error_) + "\\\"}";
}
''',
    "debug focus json",
)

# 4) The active provider category is persisted into the row state used by the renderer.
replace_once(
    "firmware/src/apps/native/M4NativeAppControllerFactory.cpp",
    '''      out.key = category.key;
      out.title = category.title;
      out.subtitle = category.subtitle;
      return true;
''',
    '''      out.key = category.key;
      out.title = category.title;
      out.subtitle = category.subtitle;
      out.value = category.key == selectedCategoryKey_ ? "selected" : "";
      return true;
''',
    "active category row state",
)

# 5) QEMU-only live QR introspection. Production debug UI still exposes only has_qr.
replace_once(
    "firmware/src/activities/apps/NativeProviderLoginActivity.cpp",
    '''std::string NativeProviderLoginActivity::debugUiJson() {
  const auto s = M4NativeProviderLogin::snapshot();
  return std::string("{\\\"kind\\\":\\\"native_provider_login\\\",\\\"provider\\\":\\\"") + providerId_ +
         "\\\",\\\"phase\\\":" + std::to_string(static_cast<int>(s.phase)) +
         ",\\\"has_qr\\\":" + (s.qrUrl.empty() ? "false" : "true") + "}";
}
''',
    '''std::string NativeProviderLoginActivity::debugUiJson() {
  const auto s = M4NativeProviderLogin::snapshot();
  std::string out = std::string("{\\\"kind\\\":\\\"native_provider_login\\\",\\\"provider\\\":\\\"") + providerId_ +
                    "\\\",\\\"phase\\\":" + std::to_string(static_cast<int>(s.phase)) +
                    ",\\\"has_qr\\\":" + (s.qrUrl.empty() ? "false" : "true");
#ifdef M4_QEMU_E2E
  if (!s.qrUrl.empty()) out += ",\\\"qr_url\\\":\\\"" + s.qrUrl + "\\\"";
#endif
  out += "}";
  return out;
}
''',
    "qemu qr debug json",
)

# Give a human enough time to scan during QEMU E2E without changing production behavior.
replace_once(
    "firmware/src/apps/providers/M4NativeProviderLogin.cpp",
    '''  const uint32_t started = millis();
  while (millis() - started < 120000u) {
''',
    '''  const uint32_t started = millis();
#ifdef M4_QEMU_E2E
  constexpr uint32_t kLoginTimeoutMs = 10u * 60u * 1000u;
#else
  constexpr uint32_t kLoginTimeoutMs = 120000u;
#endif
  while (millis() - started < kLoginTimeoutMs) {
''',
    "qemu login timeout",
)

print("issue25 native reader UX patch applied")
