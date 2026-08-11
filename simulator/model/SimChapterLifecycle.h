#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace m4sim {

// Minimal provider-reader chapter handoff model extracted from the failure class
// behind m4-firmware issues #4/#5. It deliberately models ownership/lifetime,
// not plugin business logic: an unready next chapter is an intent, never an
// empty-path open; each reader generation must close before the next opens.
class SimChapterLifecycle {
 public:
  enum class State {
    Idle,
    ReaderOpen,
    SwitchRequested,
    WaitingContent,
    ReadyToOpen,
    Failed,
  };

  struct Event {
    uint32_t atMs = 0;
    std::string text;
  };

  explicit SimChapterLifecycle(uint32_t watchdogBudgetMs = 5000)
      : watchdogBudgetMs_(watchdogBudgetMs) {}

  bool openInitial(uint32_t nowMs, int chapter, const std::string& path) {
    if (state_ != State::Idle) return fail(nowMs, "initial open while not idle");
    if (path.empty()) return fail(nowMs, "refused empty initial chapter path");
    currentChapter_ = chapter;
    currentPath_ = path;
    generation_++;
    state_ = State::ReaderOpen;
    emit(nowMs, "open generation=" + std::to_string(generation_) + " chapter=" +
                    std::to_string(chapter));
    return true;
  }

  bool requestSwitch(uint32_t nowMs, int chapter) {
    if (state_ != State::ReaderOpen) return fail(nowMs, "switch requested without open reader");
    if (chapter == currentChapter_) return fail(nowMs, "switch requested to current chapter");
    pendingChapter_ = chapter;
    pendingPath_.clear();
    switchRequestedAt_ = nowMs;
    state_ = State::SwitchRequested;
    emit(nowMs, "switch intent chapter=" + std::to_string(chapter));
    return true;
  }

  // Reader teardown is a required lifetime boundary. The old Txt/file/font/UI
  // generation is considered released here; content download may then proceed.
  bool readerClosed(uint32_t nowMs) {
    if (state_ != State::SwitchRequested) return fail(nowMs, "reader close without switch intent");
    currentPath_.clear();
    closedGenerations_++;
    state_ = State::WaitingContent;
    emit(nowMs, "reader closed released_generation=" + std::to_string(generation_));
    return true;
  }

  bool contentReady(uint32_t nowMs, int chapter, const std::string& path,
                    bool fullyCommitted = true) {
    if (state_ != State::WaitingContent) return fail(nowMs, "content ready outside wait state");
    if (chapter != pendingChapter_) return fail(nowMs, "content chapter does not match switch intent");
    if (path.empty()) return fail(nowMs, "empty relPath must not become an open attempt");
    if (!fullyCommitted) return fail(nowMs, "partial chapter file must not become reader input");
    pendingPath_ = path;
    state_ = State::ReadyToOpen;
    emit(nowMs, "content ready chapter=" + std::to_string(chapter));
    return true;
  }

  bool openReady(uint32_t nowMs) {
    if (state_ != State::ReadyToOpen) return fail(nowMs, "open-ready called before content commit");
    if (pendingPath_.empty()) return fail(nowMs, "ready state has empty path");
    currentChapter_ = pendingChapter_;
    currentPath_ = pendingPath_;
    pendingChapter_ = -1;
    pendingPath_.clear();
    generation_++;
    state_ = State::ReaderOpen;
    emit(nowMs, "open generation=" + std::to_string(generation_) + " chapter=" +
                    std::to_string(currentChapter_));
    return true;
  }

  bool checkWatchdog(uint32_t nowMs) {
    if ((state_ == State::SwitchRequested || state_ == State::WaitingContent ||
         state_ == State::ReadyToOpen) &&
        nowMs - switchRequestedAt_ > watchdogBudgetMs_) {
      return fail(nowMs, "chapter transition exceeded watchdog budget");
    }
    return true;
  }

  State state() const { return state_; }
  int currentChapter() const { return currentChapter_; }
  int generation() const { return generation_; }
  int closedGenerations() const { return closedGenerations_; }
  const std::string& currentPath() const { return currentPath_; }
  const std::vector<Event>& events() const { return events_; }
  const std::vector<std::string>& errors() const { return errors_; }
  bool ok() const { return errors_.empty(); }

 private:
  void emit(uint32_t nowMs, std::string text) { events_.push_back({nowMs, std::move(text)}); }
  bool fail(uint32_t nowMs, const std::string& text) {
    errors_.push_back(text);
    state_ = State::Failed;
    emit(nowMs, "FAIL " + text);
    return false;
  }

  uint32_t watchdogBudgetMs_ = 5000;
  uint32_t switchRequestedAt_ = 0;
  State state_ = State::Idle;
  int currentChapter_ = -1;
  int pendingChapter_ = -1;
  int generation_ = 0;
  int closedGenerations_ = 0;
  std::string currentPath_;
  std::string pendingPath_;
  std::vector<Event> events_;
  std::vector<std::string> errors_;
};

}  // namespace m4sim
