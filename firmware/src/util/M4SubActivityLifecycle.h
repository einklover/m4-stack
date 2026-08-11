#pragma once

// Host-testable two-phase child activity close (no Arduino).
// Models: child.loop() may request close; parent destroys only after loop returns.

namespace M4SubActivityLifecycle {

struct Child {
  int frames = 0;
  bool closeRequested = false;
  bool destroyed = false;

  void loop() {
    if (destroyed) return;
    ++frames;
    // Simulate: request close from inside loop (like onGoBack callback).
    if (frames >= 2) closeRequested = true;
  }
};

struct Parent {
  Child* child = nullptr;
  bool pendingExit = false;
  int destroyCount = 0;

  void requestExit() { pendingExit = true; }

  // One parent frame. Returns true if child was destroyed this frame.
  bool pump() {
    if (!child || child->destroyed) return false;
    child->loop();
    // Observe close request only AFTER loop returned.
    if (child->closeRequested) pendingExit = true;
    if (pendingExit) {
      pendingExit = false;
      child->destroyed = true;
      ++destroyCount;
      // In production: unique_ptr reset happens here, after loop returns.
      return true;
    }
    return false;
  }
};

// Returns true if the lifecycle never destroys while "in" child.loop.
inline bool verifyDeferredDestroy() {
  Parent p;
  Child c;
  p.child = &c;
  // Frame 1: no close
  if (p.pump()) return false;
  if (c.destroyed || c.frames != 1) return false;
  // Frame 2: child requests close; destroy after loop
  if (!p.pump()) return false;
  if (!c.destroyed || p.destroyCount != 1) return false;
  if (c.frames != 2) return false;
  return true;
}

// Nested menu case: outer reader owns a menu child; menu callbacks only request
// deferred close (never destroy inline). Parent applies teardown after menu.loop.
struct MenuChild {
  int frames = 0;
  bool closeRequested = false;
  bool destroyed = false;
  bool destroyedWhileInLoop = false;
  bool inLoop = false;

  void requestCloseFromCallback() {
    // Models EpubReaderMenuActivity onBack/onAction → requestExitSubActivity.
    closeRequested = true;
  }

  void loop() {
    if (destroyed) return;
    inLoop = true;
    ++frames;
    if (frames >= 2) requestCloseFromCallback();
    // If parent destroyed us mid-loop this would flip destroyedWhileInLoop.
    if (destroyed) destroyedWhileInLoop = true;
    inLoop = false;
  }
};

struct ReaderParent {
  MenuChild* menu = nullptr;
  bool pendingExitMenu = false;
  int destroyCount = 0;
  bool appliedDeferredAfterLoop = false;

  bool pump() {
    if (!menu || menu->destroyed) return false;
    menu->loop();
    // Observe only after loop returned.
    if (menu->closeRequested) pendingExitMenu = true;
    if (pendingExitMenu) {
      pendingExitMenu = false;
      if (menu->inLoop) menu->destroyedWhileInLoop = true;
      menu->destroyed = true;
      ++destroyCount;
      appliedDeferredAfterLoop = true;
      return true;
    }
    return false;
  }
};

inline bool verifyNestedMenuDeferredClose() {
  ReaderParent reader;
  MenuChild menu;
  reader.menu = &menu;
  if (reader.pump()) return false;
  if (menu.destroyed || menu.frames != 1) return false;
  if (!reader.pump()) return false;
  if (!menu.destroyed || reader.destroyCount != 1) return false;
  if (menu.destroyedWhileInLoop) return false;
  if (!reader.appliedDeferredAfterLoop) return false;
  return true;
}

}  // namespace M4SubActivityLifecycle
