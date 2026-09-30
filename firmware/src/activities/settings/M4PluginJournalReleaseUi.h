#pragma once

// Host-testable session for the settings page that releases one stuck plugin
// install journal. Opening the page and moving the selection never request a
// release. Only activate() while the confirm page is showing does.

#include <cstdint>

enum class M4JournalReleasePage : uint8_t { List = 0, Confirm = 1, Result = 2 };

struct M4JournalReleaseUi {
  M4JournalReleasePage page = M4JournalReleasePage::List;
  int selected = 0;
  int count = 0;
  bool releaseRequested = false;
  int releaseIndex = -1;
  bool leave = false;
};

inline void m4JournalReleaseSetCount(M4JournalReleaseUi& ui, int count) {
  ui.count = count < 0 ? 0 : count;
  if (ui.count == 0) ui.selected = 0;
  else if (ui.selected >= ui.count) ui.selected = ui.count - 1;
  else if (ui.selected < 0) ui.selected = 0;
}

inline void m4JournalReleaseMove(M4JournalReleaseUi& ui, int delta) {
  if (ui.page != M4JournalReleasePage::List || ui.count <= 0 || delta == 0) return;
  int next = ui.selected + delta;
  if (next < 0) next = 0;
  if (next >= ui.count) next = ui.count - 1;
  ui.selected = next;
}

// List activate opens confirm and does not request a release.
// Confirm activate is the only path that requests a release.
inline void m4JournalReleaseActivate(M4JournalReleaseUi& ui) {
  if (ui.page == M4JournalReleasePage::List) {
    if (ui.count <= 0) return;
    ui.page = M4JournalReleasePage::Confirm;
    ui.releaseRequested = false;
    ui.releaseIndex = -1;
    return;
  }
  if (ui.page == M4JournalReleasePage::Confirm) {
    ui.releaseRequested = true;
    ui.releaseIndex = ui.selected;
  }
}

// Confirm/result cancel returns to the list without a release request.
// List cancel leaves the page.
inline void m4JournalReleaseCancel(M4JournalReleaseUi& ui) {
  ui.releaseRequested = false;
  ui.releaseIndex = -1;
  if (ui.page == M4JournalReleasePage::List) {
    ui.leave = true;
    return;
  }
  ui.page = M4JournalReleasePage::List;
}

inline void m4JournalReleaseShowResult(M4JournalReleaseUi& ui) {
  ui.page = M4JournalReleasePage::Result;
  ui.releaseRequested = false;
  ui.releaseIndex = -1;
}

// First visible row for a window of `visible` lines. Selection past the first
// screen stays reachable; the activity stores every pending id.
inline int m4JournalReleaseWindowStart(const M4JournalReleaseUi& ui, int visible) {
  if (visible < 1) visible = 1;
  if (ui.count <= visible) return 0;
  int start = ui.selected - (visible - 1);
  if (start < 0) start = 0;
  const int maxStart = ui.count - visible;
  if (start > maxStart) start = maxStart;
  return start;
}
