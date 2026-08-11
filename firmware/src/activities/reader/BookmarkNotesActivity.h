#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "BookmarkStore.h"
#include "../ActivityWithSubactivity.h"

struct BookmarkNoteItem {
  bool isSeparator;         // true=书名分隔行, false=书签项
  std::string displayText;  // 分隔行显示书名，书签项显示标题
  std::string bookPath;     // 书籍路径（仅书签项有效）
  float percentage;          // 书签百分比（仅书签项有效）
};

class BookmarkNotesActivity final : public ActivityWithSubactivity {
 public:
  explicit BookmarkNotesActivity(
      GfxRenderer& renderer, MappedInputManager& mappedInput, const std::function<void()>& onGoBack,
      const std::function<void(const std::string& bookPath, float percentage)>& onOpenBookAtBookmark)
      : ActivityWithSubactivity("BookmarkNotes", renderer, mappedInput),
        onGoBack(onGoBack),
        onOpenBookAtBookmark(onOpenBookAtBookmark) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  std::vector<BookmarkNoteItem> items;
  int selectorIndex = 0;
  bool updateRequired = false;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;

  const std::function<void()> onGoBack;
  const std::function<void(const std::string& bookPath, float percentage)> onOpenBookAtBookmark;

  int getPageItems() const;
  void moveToNextSelectable(int direction);
  void buildItems();
  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();
};
