#pragma once

// Tiny display-ownership hook so GfxRenderer can refuse a refresh while
// Browser Bridge owns the panel. Implemented by M4B3Panel.cpp on Murphy.

#if defined(CROSSPOINT_MURPHY_M4)
bool m4BrowserBridgeOwnsDisplay();
bool m4BrowserBridgeIsPresenting();
void m4BrowserBridgeSetPresenting(bool presenting);
void m4BrowserBridgeInvalidatePhysicalBaseline();
void m4BrowserBridgeNotePanelReinit();
#else
inline bool m4BrowserBridgeOwnsDisplay() { return false; }
inline bool m4BrowserBridgeIsPresenting() { return false; }
inline void m4BrowserBridgeSetPresenting(bool) {}
inline void m4BrowserBridgeInvalidatePhysicalBaseline() {}
inline void m4BrowserBridgeNotePanelReinit() {}
#endif
