#pragma once

#include <cstdint>

#include "activities/settings/M4SettingsFocus.h"
#include "activities/settings/M4SettingsRootUi.h"

// Settings-owned refresh-rect / partial-render seam.
// Full vs Partial is distinct: first paint and burst-6 are Full (displayBuffer);
// old∪new row moves are Partial with a screen-absolute union rect submitted
// through GfxRenderer::displayWindow.

enum class M4SettingsRefreshMode : uint8_t { Full, Partial };

struct M4SettingsRefreshRect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

struct M4SettingsRefreshRequest {
  M4SettingsRefreshMode mode = M4SettingsRefreshMode::Full;
  M4SettingsRefreshRect rect{};
};

struct M4SettingsDisplayPort {
  void (*submitFull)(void* ctx);
  void (*submitPartial)(void* ctx, int x, int y, int w, int h);
  void* ctx;
};

inline void m4SettingsDisplaySubmit(const M4SettingsDisplayPort& port, const M4SettingsRefreshRequest& req) {
  if (req.mode == M4SettingsRefreshMode::Full) {
    if (port.submitFull) port.submitFull(port.ctx);
    return;
  }
  if (port.submitPartial) {
    port.submitPartial(port.ctx, req.rect.x, req.rect.y, req.rect.w, req.rect.h);
  }
}

struct M4SettingsPaintSession {
  bool firstPaint = true;
  M4DirtyUnion pending{0, 0, true};
};

inline M4SettingsRefreshRequest m4SettingsMakeRefresh(const M4DirtyUnion& dirty, int originY, int screenW,
                                                      int screenH, bool forceFull) {
  M4SettingsRefreshRequest r;
  if (forceFull || dirty.full) {
    r.mode = M4SettingsRefreshMode::Full;
    r.rect = {0, 0, screenW, screenH};
    return r;
  }
  int y0 = originY + dirty.y0;
  int y1 = originY + dirty.y1;
  if (y0 > y1) {
    const int tmp = y0;
    y0 = y1;
    y1 = tmp;
  }
  if (y0 < 0) y0 = 0;
  if (y1 < 0) y1 = 0;
  if (screenH > 0) {
    if (y0 > screenH) y0 = screenH;
    if (y1 > screenH) y1 = screenH;
  }
  if (screenW <= 0 || screenH <= 0 || y1 <= y0) {
    r.mode = M4SettingsRefreshMode::Full;
    r.rect = {0, 0, screenW, screenH};
    return r;
  }
  r.mode = M4SettingsRefreshMode::Partial;
  r.rect = {0, y0, screenW, y1 - y0};
  return r;
}

inline M4DirtyUnion m4SettingsPaintNoteMove(M4SettingsUiState& st, M4SettingsPaintSession& paint, int oldSlot,
                                           int newSlot, int itemH, int gap, int oldWindowStart = -1) {
  paint.pending = m4SettingsUiNoteMove(st, oldSlot, newSlot, itemH, gap, oldWindowStart);
  return paint.pending;
}

inline M4DirtyUnion m4SettingsPaintNoteMoveRoot(M4SettingsUiState& st, M4SettingsPaintSession& paint, int oldSlot,
                                               int newSlot) {
  paint.pending = m4SettingsUiNoteMoveRoot(st, oldSlot, newSlot);
  return paint.pending;
}

inline M4SettingsRefreshRequest m4SettingsPaintTake(M4SettingsPaintSession& paint, int originY, int screenW,
                                                    int screenH) {
  const M4SettingsRefreshRequest req =
      m4SettingsMakeRefresh(paint.pending, originY, screenW, screenH, paint.firstPaint);
  paint.firstPaint = false;
  paint.pending = M4DirtyUnion{0, 0, true};
  return req;
}

inline void m4SettingsPaintResetFull(M4SettingsPaintSession& paint) {
  paint.firstPaint = true;
  paint.pending = M4DirtyUnion{0, 0, true};
}

inline void m4SettingsPaintResetFull(M4SettingsPaintSession& paint, M4SettingsUiState& st) {
  st.burstCount = 0;
  m4SettingsPaintResetFull(paint);
}
