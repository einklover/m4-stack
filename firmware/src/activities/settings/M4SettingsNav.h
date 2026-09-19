#pragma once

// Clamp Up/Down. Do not wrap. Does not mutate settingsNavMoveRow (Hub tests).

inline int m4SettingsNavMoveClamp(int selected, int delta, int count) {
  if (count <= 0) return 0;
  if (selected < 0) selected = 0;
  if (selected >= count) selected = count - 1;
  if (delta == 0) return selected;
  const int next = selected + delta;
  if (next < 0) return 0;
  if (next >= count) return count - 1;
  return next;
}
