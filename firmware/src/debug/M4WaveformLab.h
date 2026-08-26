#pragma once

// M4 Waveform Lab: USB-driven SSD1677 LUT experimentation.
// Hidden feature — only reachable over the debug bridge (no UI entry).
//
// Commands (all plain JSON over the M4SerialDebug bridge):
//   lut_begin  {slot:0|1, size:48000}            start frame upload (chunks follow)
//   lut_upload {lut:"<b64 110 bytes>"}            set the experiment LUT
//   lut_set_frames {prev:"path", next:"path"}     SD frame paths (run reads from SD)
//   lut_run    {mode:"fast"}                      refresh prev->next with current LUT
//   lut_swap                                     swap slots (next becomes prev)
//   lut_stats                                    {last_ms, lut_set, frames}
//   lut_clear  {mode:"fast"}                      safe recovery refresh
//   lut_end                                      leave experiment mode
//
// Chunks uploaded after lut_begin land in the PSRAM frame slot; the bridge's
// chunk session writes here instead of SD. Safety: voltage bytes (105..109)
// are force-kept at the board's factory values unless explicitly unlocked;
// all lab refreshes are normalized to the standard fast waveform.

#include <cstddef>
#include <cstdint>

class HalDisplay;

namespace M4WaveformLab {

inline constexpr size_t kFrameBytes = 48000;  // 800x480 1bpp
inline constexpr size_t kLutBytes = 110;

struct Stats {
  uint32_t lastRunMs = 0;
  uint32_t runs = 0;
  bool lutSet = false;
  bool active = false;
  bool framesReady = false;
};

// Call from the debug bridge.  Returns a JSON error key or nullptr on success.
// `display` must be the live HalDisplay (NULL when not available).
void setDisplay(HalDisplay* display);
// Directly set the experiment LUT from a 110-byte buffer (voltage tail
// locked to the safe factory values).  Returns false on bad length.
bool setLutBytes(const uint8_t* lut, size_t len);
bool beginFrameUpload(int slot);
uint8_t* frameSlot(int slot);          // nullptr when slot not ready
bool frameUploadComplete(int slot);    // size matched
// Bridge chunk path: append one chunk to the active upload slot.
bool frameChunkAppend(const uint8_t* data, size_t len);
void endFrameUpload(int slot);
void swapSlots();
// SD-backed frames: run reads prev/next from SD instead of uploaded slots.
bool setSdFrames(const char* prevPath, const char* nextPath);
// Establish the physical baseline with a fast refresh to the given SD frame so
// subsequent differential runs start from a known state.
bool baselineFromSd(const char* framePath);
// Kindle-style multipass wipe (full-frame fast updates):
//   each step: RED = original page1, BW = composeWipe(page1, page2, edge)
// Covered region keeps being driven page1→page2 every step (ink settles);
// uncovered stays page1. New page enters from the right; `feather` dithers
// the edge. Returns total ms, or 0 on failure.
uint32_t runAnimate(const char* prevPath, const char* nextPath, int steps, int feather, uint32_t tailMs,
                    int dir);
// In-memory page-turn animation: old/new are full physical frames (48000B,
// 0=black 1=white) already in RAM (e.g. renderer.frameBuffer).  Runs the
// same multipass wipe as runAnimate but without SD access.  Returns ms.
// bodyClip (optional, byte-aligned x/w) limits every write AND the activation
// scan to the body rectangle, so status/other regions are never re-driven
// (local page-turn refresh; the wipe window walks the body fully).
struct BodyClip {
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  bool active() const { return w > 0 && h > 0; }
};
uint32_t runAnimateMem(const uint8_t* oldFrame, const uint8_t* newFrame, int steps, int feather,
                       uint32_t tailMs, int dir, BodyClip bodyClip = {});
// Sliding-window partial wipe (SD frames): steps advances = steps refreshes,
// then walk-off drain until the mult-wide trailing edge clears the panel
// (~winMult more activates; no off-panel geometry). winMult = window width
// in step units. dir: 0=R→L 1=L→R 2=B→T 3=T→B.
uint32_t runAnimateWindow(const char* prevPath, const char* nextPath, int steps, uint32_t tailMs,
                          int winMult, int dir = 0);
// In-memory sliding-window partial wipe (reader page-turn path). The trailing
// window walk-off is the settle; there is no separate tail refresh.
uint32_t runAnimateMemWindow(const uint8_t* oldFrame, const uint8_t* newFrame, int steps,
                             int winMult, int dir = 0);
bool startAnimateWindow(const char* prevPath, const char* nextPath, int steps, int winMult,
                        int dir = 0);
bool startAnimate(const char* prevPath, const char* nextPath, int steps, int feather, bool windowMode,
                  int winMult = 1, int dir = 0);
bool pumpAnimateWindow(uint32_t& stepMsOut);
bool animateActive();
// Post-animation settle: one final fast differential update. Caller-supplied
// LUTs are ignored by the display facade, so this cannot arm a multi-phase
// waveform. Returns ms or 0.
uint32_t runSettle(const char* prevPath, const char* nextPath);
bool setLut(const uint8_t* lut, size_t len, bool unlockVoltages);
uint32_t runRefresh(bool swapAfter);   // returns last run ms (0 on failure)
void clearAll();                       // safe fast refresh + reset state
Stats stats();

}  // namespace M4WaveformLab
