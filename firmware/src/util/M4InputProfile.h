#pragma once

// Platform input capability profile (host-testable, no Arduino deps required
// for unit tests that set the override).

namespace M4InputProfile {

struct Caps {
  bool hasTouch = false;
  bool hasPhysicalNavKeys = false;
  bool showHardwareKeyHints = false;
};

// Production defaults from compile-time platform.
inline Caps defaultCaps() {
  Caps c;
#if defined(CROSSPOINT_MURPHY_M4)
  c.hasTouch = true;
  c.hasPhysicalNavKeys = false;
  c.showHardwareKeyHints = false;
#elif defined(CROSSPOINT_X3) || defined(CROSSPOINT_X4)
  c.hasTouch = true;  // may still be false at runtime if no panel
  c.hasPhysicalNavKeys = true;
  c.showHardwareKeyHints = true;
#else
  // Simulator / generic host: touch-first, no HW key hints by default.
  c.hasTouch = true;
  c.hasPhysicalNavKeys = false;
  c.showHardwareKeyHints = false;
#endif
  return c;
}

// Optional test override (nullptr = use defaultCaps).
inline Caps* &overridePtr() {
  static Caps* p = nullptr;
  return p;
}

inline void setOverride(Caps* p) { overridePtr() = p; }
inline void clearOverride() { overridePtr() = nullptr; }

inline Caps caps() {
  if (overridePtr()) return *overridePtr();
  return defaultCaps();
}

inline bool hasTouch() { return caps().hasTouch; }
inline bool hasPhysicalNavKeys() { return caps().hasPhysicalNavKeys; }
inline bool showHardwareKeyHints() { return caps().showHardwareKeyHints; }

// Footer/hint copy helpers — never imply a physical bottom key row on M4.
inline const char* readerFooterHint() {
  if (showHardwareKeyHints()) return "Confirm=menu  Back=exit";
  if (hasTouch()) return "Tap sides=page  Center=menu  Swipe back=exit";
  return "Navigate";
}

inline const char* pluginBackHint() {
  if (showHardwareKeyHints()) return "tap/back";
  if (hasTouch()) return "tap to continue";
  return "continue";
}

}  // namespace M4InputProfile
