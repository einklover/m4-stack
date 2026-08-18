#pragma once

// Tiny display-ownership hook so GfxRenderer can refuse a refresh while
// Browser Bridge owns the panel. Implemented by M4B3Panel.cpp on Murphy.

#if defined(CROSSPOINT_MURPHY_M4)
bool m4BrowserBridgeOwnsDisplay();
void m4BrowserBridgeInvalidatePhysicalBaseline();
void m4BrowserBridgeNotePanelReinit();
#else
inline bool m4BrowserBridgeOwnsDisplay() { return false; }
inline void m4BrowserBridgeInvalidatePhysicalBaseline() {}
inline void m4BrowserBridgeNotePanelReinit() {}
#endif
