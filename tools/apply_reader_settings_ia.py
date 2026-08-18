#!/usr/bin/env python3
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
HDR = ROOT / "firmware/src/activities/reader/EpubReaderMenuActivity.h"
CPP = ROOT / "firmware/src/activities/reader/EpubReaderMenuActivity.cpp"
E2E = ROOT / "simulator/tests/reader_ui_e2e.py"


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one match, found {count}")
    return text.replace(old, new, 1)


def regex_once(text: str, pattern: str, repl: str, label: str) -> str:
    out, count = re.subn(pattern, repl, text, count=1, flags=re.S)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one regex match, found {count}")
    return out


# --- EpubReaderMenuActivity.h -------------------------------------------------
h = HDR.read_text(encoding="utf-8")
h = replace_once(
    h,
    '''  enum class InternalAction : uint8_t {\n    NONE = 0,\n    OPEN_STYLE,\n    OPEN_MORE,\n    FONT_DECREASE,\n    FONT_INCREASE,\n    LAYOUT_COMPACT,\n    LAYOUT_STANDARD,\n    LAYOUT_RELAXED,\n  };''',
    '''  enum class MoreSection : uint8_t { ROOT = 0, TYPOGRAPHY, TURNING, DISPLAY, CONTROL, DATA };\n\n  enum class InternalAction : uint8_t {\n    NONE = 0,\n    OPEN_STYLE,\n    OPEN_MORE,\n    OPEN_MORE_TYPOGRAPHY,\n    OPEN_MORE_TURNING,\n    OPEN_MORE_DISPLAY,\n    OPEN_MORE_CONTROL,\n    OPEN_MORE_DATA,\n    FONT_DECREASE,\n    FONT_INCREASE,\n    LAYOUT_COMPACT,\n    LAYOUT_STANDARD,\n    LAYOUT_RELAXED,\n  };''',
    "header internal actions",
)

h = replace_once(
    h,
    '''    for (const auto& item : moreMenuItems) {\n      if (item.action == MenuAction::SYNC || item.action == MenuAction::SYNCY) {''',
    '''    for (const auto& item : dataMenuItems) {\n      if (item.action == MenuAction::SYNC || item.action == MenuAction::SYNCY) {''',
    "debug sync source",
)

h = replace_once(
    h,
    '''    out += ",\\\"overlay\\\":";\n    out += menuLayer_ == MenuLayer::MORE ? "false" : "true";\n    if (menuLayer_ == MenuLayer::STYLE) {''',
    '''    out += ",\\\"overlay\\\":";\n    out += menuLayer_ == MenuLayer::MORE ? "false" : "true";\n    if (menuLayer_ == MenuLayer::MORE) {\n      out += ",\\\"more_section\\\":\\\"";\n      out += moreSectionKey();\n      out += "\\\"";\n    }\n    if (menuLayer_ == MenuLayer::STYLE) {''',
    "debug more section",
)

h = regex_once(
    h,
    r'''  std::vector<MenuItem> moreMenuItems = \{.*?\n  \};\n\n  MenuLayer menuLayer_ = MenuLayer::QUICK;''',
    '''  const std::vector<MenuItem> moreRootItems = {\n      {MenuAction::READER_SETTINGS, "排版与字体", InternalAction::OPEN_MORE_TYPOGRAPHY},\n      {MenuAction::AUTO_PAGE_TURN, "翻页与自动", InternalAction::OPEN_MORE_TURNING},\n      // Keep bookmark management one tap away; it is content, not a device setting.\n      {MenuAction::BOOKMARK_MANAGER, "书签管理"},\n      {MenuAction::TOGGLE_ANTI_ALIAS, "显示", InternalAction::OPEN_MORE_DISPLAY},\n      {MenuAction::TOGGLE_GLOBAL_NEXT_PAGE, "操作控制", InternalAction::OPEN_MORE_CONTROL},\n      {MenuAction::DELETE_CACHE, "数据与缓存", InternalAction::OPEN_MORE_DATA},\n  };\n\n  const std::vector<MenuItem> typographyMenuItems = {\n      {MenuAction::SELECT_EXTERNAL_FONT, "全部字体"},\n      {MenuAction::READER_SETTINGS, "排版与页面"},\n  };\n\n  const std::vector<MenuItem> turningMenuItems = {\n      {MenuAction::ROTATE_SCREEN, "阅读方向"},\n      {MenuAction::AUTO_PAGE_TURN, "自动翻页"},\n      {MenuAction::PAGE_TURN_INTERVAL, "自动间隔"},\n      {MenuAction::PAGE_TURN_MODE, "自动方式"},\n  };\n\n  const std::vector<MenuItem> displayMenuItems = {\n      {MenuAction::TOGGLE_ANTI_ALIAS, "抗锯齿"},\n      {MenuAction::TOGGLE_DARK_MODE, "暗黑模式"},\n  };\n\n  const std::vector<MenuItem> controlMenuItems = {\n      {MenuAction::TOGGLE_GLOBAL_NEXT_PAGE, "全局下一页"},\n      {MenuAction::LONG_PRESS_CONFIRM_MAPPING, "长按确认键"},\n#ifdef CROSSPOINT_X3\n      {MenuAction::TILT_PAGE_TURN, "晃动翻页"},\n      {MenuAction::TILT_PAGE_TURN_SETTINGS, "晃动设置"},\n#endif\n  };\n\n  std::vector<MenuItem> dataMenuItems = {\n      {MenuAction::SYNC, "KOReader 同步"},\n      {MenuAction::SYNCY, "开源阅读同步"},\n      {MenuAction::DELETE_CACHE, "清理当前书缓存"},\n  };\n\n  MenuLayer menuLayer_ = MenuLayer::QUICK;\n  MoreSection moreSection_ = MoreSection::ROOT;''',
    "replace flat More data",
)

h = regex_once(
    h,
    r'''  const std::vector<MenuItem>& activeMenuItems\(\) const \{\n    if \(menuLayer_ == MenuLayer::STYLE\) return styleMenuItems;\n    return menuLayer_ == MenuLayer::QUICK \? quickMenuItems : moreMenuItems;\n  \}''',
    '''  const std::vector<MenuItem>& activeMenuItems() const {\n    if (menuLayer_ == MenuLayer::STYLE) return styleMenuItems;\n    if (menuLayer_ == MenuLayer::QUICK) return quickMenuItems;\n    switch (moreSection_) {\n      case MoreSection::TYPOGRAPHY: return typographyMenuItems;\n      case MoreSection::TURNING: return turningMenuItems;\n      case MoreSection::DISPLAY: return displayMenuItems;\n      case MoreSection::CONTROL: return controlMenuItems;\n      case MoreSection::DATA: return dataMenuItems;\n      case MoreSection::ROOT:\n      default: return moreRootItems;\n    }\n  }''',
    "active menu source",
)

h = replace_once(
    h,
    '''  void applyQuickFontChoice(int slot);\n};''',
    '''  void applyQuickFontChoice(int slot);\n  void showMoreRoot();\n  const char* moreSectionKey() const;\n  const char* moreSectionTitle() const;\n};''',
    "header More helpers",
)
HDR.write_text(h, encoding="utf-8")


# --- EpubReaderMenuActivity.cpp ----------------------------------------------
c = CPP.read_text(encoding="utf-8")
c = replace_once(
    c,
    '''  menuLayer_ = initialLayer_;\n  selectedIndex = 0;''',
    '''  menuLayer_ = initialLayer_;\n  moreSection_ = MoreSection::ROOT;\n  selectedIndex = 0;''',
    "onEnter reset section",
)

c = replace_once(
    c,
    '''    moreMenuItems.erase(\n        std::remove_if(moreMenuItems.begin(), moreMenuItems.end(), [](const MenuItem& item) {\n          return item.action == MenuAction::SYNC || item.action == MenuAction::SYNCY;\n        }),\n        moreMenuItems.end());''',
    '''    dataMenuItems.erase(\n        std::remove_if(dataMenuItems.begin(), dataMenuItems.end(), [](const MenuItem& item) {\n          return item.action == MenuAction::SYNC || item.action == MenuAction::SYNCY;\n        }),\n        dataMenuItems.end());''',
    "capability-filter data section",
)

# The quick font row should reflect both legacy /fonts/*.epdfont and /FONT runtime sfnt.
c = replace_once(c, "getAvailableTtfFamilies()", "getAvailableFamilies()", "all available font families")

c = replace_once(
    c,
    '''  if (action == InternalAction::OPEN_MORE) {\n    menuLayer_ = MenuLayer::MORE;\n    selectedIndex = 0;\n    updateRequired = true;\n    return;\n  }''',
    '''  if (action == InternalAction::OPEN_MORE) {\n    menuLayer_ = MenuLayer::MORE;\n    moreSection_ = MoreSection::ROOT;\n    selectedIndex = 0;\n    updateRequired = true;\n    return;\n  }\n\n  if (action == InternalAction::OPEN_MORE_TYPOGRAPHY ||\n      action == InternalAction::OPEN_MORE_TURNING ||\n      action == InternalAction::OPEN_MORE_DISPLAY ||\n      action == InternalAction::OPEN_MORE_CONTROL ||\n      action == InternalAction::OPEN_MORE_DATA) {\n    menuLayer_ = MenuLayer::MORE;\n    if (action == InternalAction::OPEN_MORE_TYPOGRAPHY) moreSection_ = MoreSection::TYPOGRAPHY;\n    else if (action == InternalAction::OPEN_MORE_TURNING) moreSection_ = MoreSection::TURNING;\n    else if (action == InternalAction::OPEN_MORE_DISPLAY) moreSection_ = MoreSection::DISPLAY;\n    else if (action == InternalAction::OPEN_MORE_CONTROL) moreSection_ = MoreSection::CONTROL;\n    else moreSection_ = MoreSection::DATA;\n    selectedIndex = 0;\n    updateRequired = true;\n    return;\n  }''',
    "open More sections",
)

c = replace_once(
    c,
    '''    case InternalAction::OPEN_STYLE:\n    case InternalAction::OPEN_MORE:\n      return ">";''',
    '''    case InternalAction::OPEN_MORE_TYPOGRAPHY:\n      return "字体 · 段落 · 页面";\n    case InternalAction::OPEN_MORE_TURNING:\n      return SETTINGS.autoPageTurnEnabled ? "自动开启" : "方向 · 自动";\n    case InternalAction::OPEN_MORE_DISPLAY:\n      return SETTINGS.textAntiAliasing ? "抗锯齿开" : "抗锯齿关";\n    case InternalAction::OPEN_MORE_CONTROL:\n      return SETTINGS.globalNextPageModeEnabled ? "全局下一页开" : "按键 · 快捷操作";\n    case InternalAction::OPEN_MORE_DATA: {\n      const bool hasSync = std::any_of(dataMenuItems.begin(), dataMenuItems.end(), [](const MenuItem& item) {\n        return item.action == MenuAction::SYNC || item.action == MenuAction::SYNCY;\n      });\n      return hasSync ? "同步 · 缓存" : "缓存";\n    }\n    case InternalAction::OPEN_STYLE:\n    case InternalAction::OPEN_MORE:\n      return ">";''',
    "More root summaries",
)

c = replace_once(
    c,
    '''void EpubReaderMenuActivity::closeToReader() {\n  onBack(pendingOrientation);\n  notifyParentStyleChanged();\n}''',
    '''void EpubReaderMenuActivity::showMoreRoot() {\n  moreSection_ = MoreSection::ROOT;\n  selectedIndex = 0;\n  updateRequired = true;\n}\n\nconst char* EpubReaderMenuActivity::moreSectionKey() const {\n  switch (moreSection_) {\n    case MoreSection::TYPOGRAPHY: return "typography";\n    case MoreSection::TURNING: return "turning";\n    case MoreSection::DISPLAY: return "display";\n    case MoreSection::CONTROL: return "control";\n    case MoreSection::DATA: return "data";\n    case MoreSection::ROOT:\n    default: return "root";\n  }\n}\n\nconst char* EpubReaderMenuActivity::moreSectionTitle() const {\n  switch (moreSection_) {\n    case MoreSection::TYPOGRAPHY: return "排版与字体";\n    case MoreSection::TURNING: return "翻页与自动";\n    case MoreSection::DISPLAY: return "显示";\n    case MoreSection::CONTROL: return "操作控制";\n    case MoreSection::DATA: return "数据与缓存";\n    case MoreSection::ROOT:\n    default: return "更多";\n  }\n}\n\nvoid EpubReaderMenuActivity::closeToReader() {\n  onBack(pendingOrientation);\n  notifyParentStyleChanged();\n}''',
    "More helper definitions",
)

c = replace_once(
    c,
    '''    if (mappedInput.wasBackGesture()) {\n      closeToReader();\n      return;\n    }''',
    '''    if (mappedInput.wasBackGesture()) {\n      if (menuLayer_ == MenuLayer::MORE && moreSection_ != MoreSection::ROOT) {\n        showMoreRoot();\n      } else {\n        closeToReader();\n      }\n      return;\n    }''',
    "touch back hierarchy",
)

c = replace_once(
    c,
    '''  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {\n    closeToReader();\n    return;\n  }''',
    '''  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {\n    if (menuLayer_ == MenuLayer::MORE && moreSection_ != MoreSection::ROOT) {\n      showMoreRoot();\n    } else {\n      closeToReader();\n    }\n    return;\n  }''',
    "button back hierarchy",
)

c = replace_once(
    c,
    '''  const std::string headerTitle = title + " · 更多";''',
    '''  const std::string headerTitle = moreSection_ == MoreSection::ROOT\n                                      ? title + " · 更多"\n                                      : std::string(moreSectionTitle());''',
    "section header title",
)

c = replace_once(
    c,
    '''  const auto labels = mappedInput.mapLabels("« 阅读", "选择", "向上", "向下");''',
    '''  const auto labels = mappedInput.mapLabels(\n      menuLayer_ == MenuLayer::MORE && moreSection_ != MoreSection::ROOT ? "« 更多" : "« 阅读",\n      "选择", "向上", "向下");''',
    "section back hint",
)
CPP.write_text(c, encoding="utf-8")


# --- Reader journey: assert the new root and one real second-level section -----
t = E2E.read_text(encoding="utf-8")
t = replace_once(
    t,
    '''        if more_body.get("layer") != "more" or more_body.get("overlay") is not False or more_body.get("has_sync") is not False:\n            raise m4sim.M4SimError(f"TXT More capability contract violated: {more_body!r}")\n        more = _capture(client, root, "14-more-full-page")\n        _send_key(client, "back")\n        reader_after_more = _settle_reader(client, proc, qlog, root, "15-reader-after-more")''',
    '''        if (more_body.get("layer") != "more" or more_body.get("overlay") is not False or\n                more_body.get("has_sync") is not False or more_body.get("items") != 6 or\n                more_body.get("more_section") != "root"):\n            raise m4sim.M4SimError(f"TXT More root contract violated: {more_body!r}")\n        more = _capture(client, root, "14-more-root")\n\n        # Root item 0 opens the typography/font section inside the same Menu Activity.\n        _send_key(client, "confirm")\n        typography_ui = _wait_path(client, proc, qlog, MENU_PATH, seconds=20.0)\n        typography_body = _deepest_body(typography_ui)\n        if (typography_body.get("layer") != "more" or\n                typography_body.get("more_section") != "typography" or\n                typography_body.get("items") != 2):\n            raise m4sim.M4SimError(f"More typography section contract violated: {typography_body!r}")\n        typography = _capture(client, root, "14b-more-typography")\n        _send_key(client, "back")\n        more_root_again_ui = _wait_path(client, proc, qlog, MENU_PATH, seconds=20.0)\n        if _deepest_body(more_root_again_ui).get("more_section") != "root":\n            raise m4sim.M4SimError(f"More section Back did not return to root: {more_root_again_ui!r}")\n        more_root_again = _capture(client, root, "14c-more-root-after-back")\n        _send_key(client, "back")\n        reader_after_more = _settle_reader(client, proc, qlog, root, "15-reader-after-more")''',
    "reader E2E More hierarchy",
)

t = replace_once(
    t,
    '''        _assert_changed(more, reader_after_more, "More -> reader")''',
    '''        _assert_changed(more, typography, "More root -> typography")\n        _assert_changed(typography, more_root_again, "typography -> More root")\n        _assert_changed(more_root_again, reader_after_more, "More -> reader")''',
    "reader E2E More assertions",
)

t = replace_once(
    t,
    '''                    reader_after_progress_catalog, more, reader_after_more,\n                    bookmark_added, bookmark_list, reader_final,''',
    '''                    reader_after_progress_catalog, more, typography, more_root_again, reader_after_more,\n                    bookmark_added, bookmark_list, reader_final,''',
    "reader E2E screenshots",
)
E2E.write_text(t, encoding="utf-8")

print("reader settings IA codemod applied")
