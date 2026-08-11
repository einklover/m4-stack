#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "CrossPointSettings.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "util/M4TouchNavigation.h"
#include "util/M4UiText.h"

enum class ThemeType { Lyra, Fengyan };

class UITheme {
  static UITheme instance;

 public:
  class Facade {
   public:
    explicit Facade(const UITheme& owner) : owner_(owner) {}

    operator const BaseTheme&() const { return owner_.getTheme(); }

    void drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current, size_t total) const {
      owner_.getTheme().drawProgressBar(renderer, rect, current, total);
    }
    void drawBattery(const GfxRenderer& renderer, Rect rect, bool showPercentage = true) const {
      // Some theme code writes SMALL_FONT_ID directly instead of going through
      // M4UiText. Resolve once at the facade boundary so runtime TTF system UI
      // and Native plugins share the same fixed 18/22/26px chrome faces.
      (void)M4UiText::resolve(renderer, SMALL_FONT_ID);
      owner_.getTheme().drawBattery(renderer, rect, showPercentage);
    }
    void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                         const char* btn4, bool force = false) const {
      (void)M4UiText::resolve(renderer, SMALL_FONT_ID);
      if (M4TouchNavigation::enabled()) {
        M4TouchNavigation::drawBottomBar(renderer);
        return;
      }
      owner_.getTheme().drawButtonHints(renderer, btn1, btn2, btn3, btn4, force);
    }
    void drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn,
                             bool force = false) const {
      (void)M4UiText::resolve(renderer, SMALL_FONT_ID);
      owner_.getTheme().drawSideButtonHints(renderer, topBtn, bottomBtn, force);
    }
    void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                  const std::function<std::string(int index)>& rowTitle,
                  const std::function<std::string(int index)>& rowSubtitle,
                  const std::function<UIIcon(int index)>& rowIcon,
                  const std::function<std::string(int index)>& rowValue) const {
      (void)M4UiText::resolve(renderer, UI_10_FONT_ID);
      // Native XML historically supplied a rowValue callback even when every
      // value was empty. Fengyan interpreted callback presence as a real value
      // column and removed 100px from the title width. Probe the active row and
      // reclaim that gutter when it is not actually used.
      std::function<std::string(int)> effectiveValue = rowValue;
      if (effectiveValue && itemCount > 0) {
        const int probe = std::max(0, std::min(selectedIndex, itemCount - 1));
        if (effectiveValue(probe).empty()) effectiveValue = {};
      }

      // Text-only two-baseline rows (novel/search results) prioritize the full
      // book title. If the title needs a second visual line, that line occupies
      // the subtitle baseline; short titles keep the normal author/meta line.
      if (!rowIcon && rowSubtitle && rowTitle && itemCount > 0) {
        const int side = owner_.getMetrics().contentSidePadding;
        const int valueReserve = effectiveValue ? 100 : 0;
        const int wrapWidth = std::max(48, rect.width - side * 2 - 24 - valueReserve);
        auto wrappedTitle = [&, rowTitle](int index) -> std::string {
          const std::string raw = rowTitle(index);
          const auto lines = M4UiText::wrapLines(renderer, UI_10_FONT_ID, raw.c_str(), wrapWidth, 2);
          return lines.empty() ? raw : lines.front();
        };
        auto wrappedSubtitle = [&, rowTitle, rowSubtitle](int index) -> std::string {
          const std::string raw = rowTitle(index);
          const auto lines = M4UiText::wrapLines(renderer, UI_10_FONT_ID, raw.c_str(), wrapWidth, 2);
          if (lines.size() > 1) return lines[1];
          return rowSubtitle(index);
        };
        owner_.getTheme().drawList(renderer, rect, itemCount, selectedIndex,
                                   wrappedTitle, wrappedSubtitle, rowIcon, effectiveValue);
        return;
      }

      owner_.getTheme().drawList(renderer, rect, itemCount, selectedIndex,
                                 rowTitle, rowSubtitle, rowIcon, effectiveValue);
    }
    void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title) const {
      // Fengyan draws battery percentage with SMALL before it draws the title.
      // Resolve both stable faces up front so even the first frame after a font
      // switch uses the fixed native-size runtime TTF chrome.
      (void)M4UiText::resolve(renderer, SMALL_FONT_ID);
      (void)M4UiText::resolve(renderer, UI_12_FONT_ID);
      owner_.getTheme().drawHeader(renderer, rect, title);
      M4TouchNavigation::drawHeaderBack(renderer, rect);
    }
    void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs, bool selected) const {
      (void)M4UiText::resolve(renderer, UI_10_FONT_ID);
      owner_.getTheme().drawTabBar(renderer, rect, tabs, selected);
    }
    void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                             int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                             std::function<bool()> storeCoverBuffer) const {
      owner_.getTheme().drawRecentBookCover(renderer, rect, recentBooks, selectorIndex, coverRendered,
                                             coverBufferStored, bufferRestored, std::move(storeCoverBuffer));
    }
    void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                        const std::function<std::string(int index)>& buttonLabel,
                        const std::function<UIIcon(int index)>& rowIcon) const {
      (void)M4UiText::resolve(renderer, UI_10_FONT_ID);
      owner_.getTheme().drawButtonMenu(renderer, rect, buttonCount, selectedIndex, buttonLabel, rowIcon);
    }
    Rect drawPopup(const GfxRenderer& renderer, const char* message) const {
      (void)M4UiText::resolve(renderer, UI_10_FONT_ID);
      return owner_.getTheme().drawPopup(renderer, message);
    }
    void fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, int progress) const {
      owner_.getTheme().fillPopupProgress(renderer, layout, progress);
    }
    void drawReadingProgressBar(const GfxRenderer& renderer, size_t bookProgress) const {
      owner_.getTheme().drawReadingProgressBar(renderer, bookProgress);
    }

   private:
    const UITheme& owner_;
  };

  UITheme();
  static UITheme& getInstance() { return instance; }

  const ThemeMetrics& getMetrics() const { return *currentMetrics; }
  const BaseTheme& getTheme() const { return *currentTheme; }
  const Facade& getFacade() const { return facade; }
  ThemeType getThemeType() { return currentThemeType; }
  void reload();
  static int getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle);
  static std::string getCoverThumbPath(std::string coverBmpPath, int coverWidth, int coverHeight);
  static UIIcon getFileIcon(const std::string& filename);

 private:
  const ThemeMetrics* currentMetrics = nullptr;
  std::unique_ptr<BaseTheme> currentTheme;
  ThemeType currentThemeType = ThemeType::Fengyan;
  Facade facade{*this};
};

#if defined(CROSSPOINT_MURPHY_M4)
#define GUI UITheme::getInstance().getFacade()
#else
#define GUI UITheme::getInstance().getTheme()
#endif
