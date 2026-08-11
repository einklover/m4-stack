#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <vector>

#include "BookmarkStore.h"
#include "../ActivityWithSubactivity.h"

class BookmarkManagerActivity final : public ActivityWithSubactivity {
 public:
  explicit BookmarkManagerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                   std::vector<Bookmark> bookmarks, const std::function<void()>& onGoBack,
                                   const std::function<void(float percentage)>& onJumpToBookmark,
                                   const std::function<void(int index)>& onDeleteBookmark)
      : ActivityWithSubactivity("BookmarkManager", renderer, mappedInput),
        bookmarks(std::move(bookmarks)),
        onGoBack(onGoBack),
        onJumpToBookmark(onJumpToBookmark),
        onDeleteBookmark(onDeleteBookmark) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  std::vector<Bookmark> bookmarks;
  int selectorIndex = 0;
  bool updateRequired = false;
  bool deleteConfirmMode = false;
  bool deleteConfirmSelected = false;  // false=取消, true=是
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;

  const std::function<void()> onGoBack;
  const std::function<void(float percentage)> onJumpToBookmark;
  const std::function<void(int index)> onDeleteBookmark;

  int getPageItems() const;
  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();
};
