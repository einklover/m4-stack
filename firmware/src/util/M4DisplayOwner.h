#pragma once

// Tiny display-ownership hook so GfxRenderer can refuse a refresh while
// Browser Bridge owns the panel. Implemented by M4B3Panel.cpp on Murphy.

#if defined(CROSSPOINT_MURPHY_M4)
bool m4BrowserBridgeOwnsDisplay();
#else
inline bool m4BrowserBridgeOwnsDisplay() { return false; }
#endif
