#include "M4WaveformLab.h"

#include <HalDisplay.h>
#include <SDCardManager.h>

#include <cstring>

#if defined(ARDUINO_ARCH_ESP32)
#include "esp_heap_caps.h"
#endif

namespace M4WaveformLab {

namespace {

HalDisplay* gDisplay = nullptr;

// Two PSRAM frame slots (old / new).
uint8_t* gSlot[2] = {nullptr, nullptr};
size_t gSlotReceived[2] = {0, 0};
bool gUploadTarget = false;
int gUploadSlot = 0;

// SD-backed frames (kindle-style page-turn sequences): when set, runRefresh
// reads prev/next from SD instead of the uploaded PSRAM slots.
char gSdPrev[96] = {};
char gSdNext[96] = {};
bool gSdFramesSet = false;

// 110-byte LUT (105 waveform + 5 voltage tail).  Voltage tail locked by
// default: experiments must not move VGH/VSH/VSL/VCOM until explicitly
// unlocked (LUT editor in the host tool can still send them, but the device
// refuses by default).
uint8_t gLut[kLutBytes] = {};
bool gLutSet = false;
bool gVoltagesUnlocked = false;
bool gRunning = false;

uint32_t gLastRunMs = 0;
uint32_t gRuns = 0;

uint8_t* allocSlot() {
#if defined(ARDUINO_ARCH_ESP32)
  auto* p = static_cast<uint8_t*>(heap_caps_malloc(kFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (p) return p;
#endif
  return static_cast<uint8_t*>(malloc(kFrameBytes));
}

}  // namespace

void setDisplay(HalDisplay* display) { gDisplay = display; }

bool setLutBytes(const uint8_t* lut, size_t len) {
  if (!lut || len < 105) return false;
  return setLut(lut, len, /*unlockVoltages=*/false);
}

bool beginFrameUpload(int slot) {
  if (gRunning) return false;
  if (slot < 0 || slot > 1) return false;
  if (!gSlot[slot]) gSlot[slot] = allocSlot();
  if (!gSlot[slot]) return false;
  gSlotReceived[slot] = 0;
  gUploadTarget = true;
  gUploadSlot = slot;
  return true;
}

uint8_t* frameSlot(int slot) {
  if (slot < 0 || slot > 1) return nullptr;
  return gSlot[slot];
}

bool frameUploadComplete(int slot) {
  if (slot < 0 || slot > 1) return false;
  return gSlot[slot] && gSlotReceived[slot] == kFrameBytes;
}

// Called by the bridge chunk path: appends `data` to the active upload slot.
bool frameChunkAppend(const uint8_t* data, size_t len) {
  if (!gUploadTarget) return false;
  const int slot = gUploadSlot;
  if (!gSlot[slot]) return false;
  if (gSlotReceived[slot] + len > kFrameBytes) return false;
  std::memcpy(gSlot[slot] + gSlotReceived[slot], data, len);
  gSlotReceived[slot] += len;
  if (gSlotReceived[slot] == kFrameBytes) {
    gUploadTarget = false;  // slot complete
  }
  return true;
}

// Bridge calls this when the declared chunk total is reached.
void endFrameUpload(int slot) {
  (void)slot;
  gUploadTarget = false;
}

void swapSlots() {
  if (gSlot[0] && gSlot[1]) {
    std::swap(gSlot[0], gSlot[1]);
    gSlotReceived[0] = gSlotReceived[1] = kFrameBytes;
  }
}

bool setSdFrames(const char* prevPath, const char* nextPath) {
  if (!prevPath || !nextPath) return false;
  if (std::strlen(prevPath) >= sizeof(gSdPrev) || std::strlen(nextPath) >= sizeof(gSdNext)) return false;
  // Verify both files exist and are exactly one frame before accepting.
  FsFile f;
  if (!SDCardManager::getInstance().openFileForRead("LAB", prevPath, f)) return false;
  const size_t prevSize = static_cast<size_t>(f.size());
  f.close();
  if (prevSize != kFrameBytes) return false;
  if (!SDCardManager::getInstance().openFileForRead("LAB", nextPath, f)) return false;
  const size_t nextSize = static_cast<size_t>(f.size());
  f.close();
  if (nextSize != kFrameBytes) return false;
  std::strncpy(gSdPrev, prevPath, sizeof(gSdPrev) - 1);
  std::strncpy(gSdNext, nextPath, sizeof(gSdNext) - 1);
  gSdFramesSet = true;
  return true;
}

namespace {

// Read a full frame from SD into the PSRAM slot (allocate if needed).
bool readFrameIntoSlot(const char* path, int slot) {
  if (!gSlot[slot]) gSlot[slot] = allocSlot();
  if (!gSlot[slot]) return false;
  FsFile f;
  if (!SDCardManager::getInstance().openFileForRead("LAB", path, f)) return false;
  const size_t n = static_cast<size_t>(f.size());
  if (n != kFrameBytes) {
    f.close();
    return false;
  }
  if (f.read(gSlot[slot], kFrameBytes) != static_cast<int>(kFrameBytes)) {
    f.close();
    return false;
  }
  f.close();
  gSlotReceived[slot] = kFrameBytes;
  return true;
}

}  // namespace

bool baselineFromSd(const char* framePath) {
  if (!gDisplay || gRunning) return false;
  if (!readFrameIntoSlot(framePath, 0)) return false;
  gRunning = true;
  // Establish the baseline through the policy-guarded fast path. The panel
  // driver's old absolute/full waveform is not available to the lab.
  gDisplay->waveformLabBaseline(gSlot[0]);
  gRunning = false;
  return true;
}

bool setLut(const uint8_t* lut, size_t len, bool unlockVoltages) {
  if (!lut || len < 105) return false;
  std::memcpy(gLut, lut, kLutBytes);
  if (!unlockVoltages) {
    // Voltage bytes (105..109) locked: force the board's known-safe factory
    // tail (VGH/VSH1/VSH2/VSL/VCOM) from the stock grayscale LUT instead of
    // letting an experiment move the panel rails.  Unlock only when tuning
    // voltages deliberately.
    static const uint8_t kSafeVoltageTail[kLutBytes - 105] = {0x17, 0x41, 0xA8, 0x32, 0x30};
    std::memcpy(gLut + 105, kSafeVoltageTail, sizeof(kSafeVoltageTail));
  }
  gVoltagesUnlocked = unlockVoltages;
  gLutSet = true;
  return true;
}

namespace {

// Compose one wipe frame: right `edge` columns are new page, left is old;
// `feather` px around the edge use a sparse dithered transition so the slide
// reads smooth instead of a dense checkered fence.  edge>=W: full new page;
// edge<=0: full old page.
// dir: 0=right->left (new page enters from right), 1=left->right,
//      2=bottom->top (new page enters from bottom), 3=top->bottom.
void composeWipeDir(const uint8_t* old, const uint8_t* newFrame, uint8_t* out, int edge, int feather,
                    int widthBytes, int height, int dir) {
  const int W = widthBytes * 8;
  const int H = height;
  const bool horiz = (dir == 0 || dir == 1);
  const int span = horiz ? W : H;
  // When the edge has fully swept, the whole frame is the new page.
  if (edge <= 0) {
    std::memcpy(out, newFrame, static_cast<size_t>(widthBytes) * height);
    return;
  }
  if (edge >= span) {
    std::memcpy(out, old, static_cast<size_t>(widthBytes) * height);
    return;
  }
  for (int row = 0; row < H; ++row) {
    const int rowOff = row * widthBytes;
    for (int x = 0; x < W; ++x) {
      int v = 0;
      const int bx = x / 8;
      const int bit = 0x80 >> (x % 8);
      int pos;
      if (horiz) {
        pos = (dir == 0) ? x : (W - 1 - x);
      } else {
        pos = (dir == 2) ? row : (H - 1 - row);
      }
      const uint8_t* src = (pos < edge - feather) ? old : ((pos >= edge) ? newFrame : nullptr);
      if (src != nullptr) {
        v = (src[rowOff + bx] & bit) ? 1 : 0;
      } else {
        // Sparse blend inside the feather band.
        const int d = pos - (edge - feather);
        const uint8_t* s = ((d & 1) == 0) ? newFrame : old;
        v = (s[rowOff + bx] & bit) ? 1 : 0;
      }
      if (v) out[rowOff + bx] |= bit;
    }
  }
}

void composeWipe(const uint8_t* old, const uint8_t* newFrame, uint8_t* out, int edge, int feather,
                 int widthBytes, int height) {
  composeWipeDir(old, newFrame, out, edge, feather, widthBytes, height, /*dir=*/0);
}

}  // namespace

uint32_t runAnimateMem(const uint8_t* oldFrame, const uint8_t* newFrame, int steps, int feather,
                       uint32_t tailMs, int dir, BodyClip bodyClip) {
  if (!gDisplay) return 0;
  // Recover from a previous stuck lab session so reader page-turns never hard-lock.
  if (gRunning) gRunning = false;
  if (!gLutSet) return 0;
  if (!oldFrame || !newFrame) return 0;
  if (steps < 1) steps = 1;
  if (steps > 64) steps = 64;
  if (feather < 0) feather = 0;
  if (feather > 64) feather = 64;
  if (dir < 0 || dir > 3) dir = 0;
  if (bodyClip.active() && ((bodyClip.x % 8) != 0 || (bodyClip.w % 8) != 0)) {
    bodyClip = {};  // invalid clip → full panel fallback
  }
  // Single synth buffer only (previous code also allocated an unused 48KB spare).
  uint8_t* cur = allocSlot();
  if (!cur) return 0;
  const int W = 800;
  const int wb = W / 8;
  const int H = 480;
  const uint32_t t0 = millis();
  gRunning = true;
  // No FULL baseline here: reader already shows oldFrame. Each step rewrites
  // full RED=old / BW=composeWipe, so uncovered columns stay idle (RED==BW).
  // With a body clip both the write and the activation scan are limited to the
  // body rectangle — status/other regions are physically off-panel and never
  // re-driven (no ghosting, no full-frame refresh).
  auto driveStep = [&](const uint8_t* redPlane, const uint8_t* bwPlane) {
    if (bodyClip.active()) {
      gDisplay->waveformLabWriteDiffWindow(redPlane, bwPlane, bodyClip.x, bodyClip.y, bodyClip.w,
                                           bodyClip.h);
      gDisplay->waveformLabActivateWindow(bodyClip.x, bodyClip.y, bodyClip.w, bodyClip.h, gLut);
    } else {
      gDisplay->waveformLabRefresh(redPlane, bwPlane, gLut, /*turnOff=*/false);
    }
  };
  for (int i = 1; i <= steps; ++i) {
    std::memset(cur, 0, kFrameBytes);
    const int span = (dir == 0 || dir == 1) ? W : H;
    const int edge = span - (span * i) / steps;
    composeWipeDir(oldFrame, newFrame, cur, edge, feather, wb, H, dir);
    driveStep(oldFrame, cur);
    delay(2);
  }
  if (tailMs > 0) {
    uint32_t target = tailMs;
    if (target > 10000) target = 10000;
    const uint32_t tailStart = millis();
    int tail = 0;
    while (tail < 12 && (millis() - tailStart) < target) {
      driveStep(oldFrame, newFrame);
      ++tail;
      delay(2);
    }
  }
  // Final plane write: RED=BW=new with *stock* waveform (lut=nullptr). Custom
  // gLut here re-arms experiment DU and poisons the next reader FAST baseline.
  gDisplay->setCustomLUT(false, nullptr);
  if (bodyClip.active()) {
    gDisplay->waveformLabEqualizeWindow(newFrame, bodyClip.x, bodyClip.y, bodyClip.w, bodyClip.h);
  } else {
    gDisplay->waveformLabRefresh(newFrame, newFrame, /*lut=*/nullptr, /*turnOff=*/false);
  }
  free(cur);
  gRunning = false;
  const uint32_t total = millis() - t0;
  ++gRuns;
  gLastRunMs = total;
  return total;
}

uint32_t runAnimate(const char* prevPath, const char* nextPath, int steps, int feather, uint32_t tailMs,
                    int dir) {
  if (!gDisplay || gRunning) return 0;
  if (!gLutSet) return 0;
  if (steps < 1) steps = 1;
  if (steps > 64) steps = 64;
  if (feather < 0) feather = 0;
  if (feather > 64) feather = 64;
  if (dir < 0 || dir > 3) dir = 0;
  if (!readFrameIntoSlot(prevPath, 0)) return 0;
  if (!readFrameIntoSlot(nextPath, 1)) return 0;
  // BW plane buffer for the composed wipe frame (RED stays original page1).
  uint8_t* synth = allocSlot();
  if (!synth) return 0;

  const int W = 800;
  const int wb = W / 8;
  const int H = 480;
  const uint32_t t0 = millis();
  gRunning = true;
  // Absolute FULL to page1 first.  Without this, uncovered columns stay at
  // whatever the UI last painted (often Home), so the wipe appears to start
  // "on the desktop".
  gDisplay->waveformLabBaseline(gSlot[0]);
  // Kindle-style multipass wipe:
  //   RED  = original page1 every step (never the previous synth)
  //   BW   = composeWipe(page1, page2, edge)
  // Covered region (x >= edge): RED=page1 ≠ BW=page2 → drives EVERY step,
  // so already-wiped ink keeps settling ("变重") instead of one-shot freeze.
  // Uncovered region (x < edge): RED=BW=page1 → idle.
  // Intermediate frames always come from the ORIGINAL prev/next — never chain
  // synth→synth (that accumulates blend error / ghosting).
  for (int i = 1; i <= steps; ++i) {
    std::memset(synth, 0, kFrameBytes);
    // New page enters from the sweep direction: edge sweeps span->0.
    const int span = (dir == 0 || dir == 1) ? W : H;
    const int edge = span - (span * i) / steps;
    composeWipeDir(gSlot[0], gSlot[1], synth, edge, feather, wb, H, dir);
    gDisplay->waveformLabRefresh(gSlot[0], synth, gLut, /*turnOff=*/false);
    delay(2);  // yield WDT / other tasks
  }
  // Ghost-clearing tail: AFTER the wipe animation, keep driving the full
  // old->new differential for `tailMs` wall time (measured from the end of
  // the animation, not the start), so residual old-page ink keeps settling.
  // RED stays the ORIGINAL page1, BW is the FINAL page2 — every changed
  // pixel is re-driven each pass (multipass settling).
  if (tailMs > 0) {
    uint32_t target = tailMs;
    if (target > 10000) target = 10000;
    const uint32_t tailStart = millis();
    int tail = 0;
    while (tail < 12 && (millis() - tailStart) < target) {
      gDisplay->waveformLabRefresh(gSlot[0], gSlot[1], gLut, /*turnOff=*/false);
      ++tail;
      delay(2);
    }
#if defined(ARDUINO_ARCH_ESP32)
    Serial.printf("[LAB] ghost-clear tail passes=%d\n", tail);
#endif
  }
  // Leave RED==BW==page2 so the next non-lab FAST has a correct baseline.
  gDisplay->waveformLabRefresh(gSlot[1], gSlot[1], gLut, /*turnOff=*/false);
  free(synth);
  gRunning = false;
  const uint32_t total = millis() - t0;
  ++gRuns;
  gLastRunMs = total;
  {
    FsFile f;
    if (SDCardManager::getInstance().openFileForWrite("LAB", "/waveform/lab_diag.txt", f)) {
      char line[128];
      snprintf(line, sizeof(line),
               "animate kindle multipass steps=%d feather=%d total=%ums\n", steps, feather,
               static_cast<unsigned>(total));
      f.write(line, strlen(line));
      f.close();
    }
  }
  return total;
}

namespace {

// Async animation session (pumped from the main loop so the loop never blocks
// for the whole animation — blocking starves the WDT / e-ink tasks).
// mode: 0 = full-frame Kindle multipass (lut_animate)
// mode: 1 = sliding window wipe (lut_wipe):
//   steps     = number of advances (= number of activations)
//   widthMult = window width in step cells
//   dir       = 0 R→L, 1 L→R, 2 B→T, 3 T→B
struct StepCell {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

struct AnimSession {
  bool active = false;
  int mode = 0;
  int steps = 0;
  int cur = 0;
  int feather = 0;
  int widthMult = 1;
  int dir = 0;
  int W = 800;
  int wb = 100;
  int H = 480;
  uint8_t* synth = nullptr;
  const uint8_t* pageOld = nullptr;
  const uint8_t* pageNew = nullptr;
  StepCell band[64];
  int bandN = 0;
  uint32_t t0 = 0;
};
AnimSession gAnim;

const uint8_t* pageOld() { return gAnim.pageOld ? gAnim.pageOld : gSlot[0]; }
const uint8_t* pageNew() { return gAnim.pageNew ? gAnim.pageNew : gSlot[1]; }

void stepCell(int i, int count, int dir, StepCell& out) {
  if (count < 1) count = 1;
  if (i < 0) i = 0;
  if (i >= count) i = count - 1;
  out.x = 0;
  out.y = 0;
  out.w = gAnim.W;
  out.h = gAnim.H;
  if (dir == 0 || dir == 1) {
    const int totalBytes = gAnim.W / 8;
    int leftByte = 0, rightByte = 0;
    if (dir == 0) {
      rightByte = totalBytes - (totalBytes * i) / count;
      leftByte = totalBytes - (totalBytes * (i + 1)) / count;
    } else {
      leftByte = (totalBytes * i) / count;
      rightByte = (totalBytes * (i + 1)) / count;
    }
    out.x = leftByte * 8;
    out.w = (rightByte - leftByte) * 8;
    out.y = 0;
    out.h = gAnim.H;
  } else if (dir == 2) {
    const int bottom = gAnim.H - (gAnim.H * i) / count;
    const int top = gAnim.H - (gAnim.H * (i + 1)) / count;
    out.x = 0;
    out.w = gAnim.W;
    out.y = top;
    out.h = bottom - top;
  } else {
    const int top = (gAnim.H * i) / count;
    const int bottom = (gAnim.H * (i + 1)) / count;
    out.x = 0;
    out.w = gAnim.W;
    out.y = top;
    out.h = bottom - top;
  }
  if (out.w < 0) out.w = 0;
  if (out.h < 0) out.h = 0;
}

void bandPopFront() {
  if (gAnim.bandN <= 0) return;
  for (int i = 1; i < gAnim.bandN; ++i) gAnim.band[i - 1] = gAnim.band[i];
  --gAnim.bandN;
}

void equalizeFrontCell() {
  if (gAnim.bandN <= 0) return;
  const StepCell& s = gAnim.band[0];
  if (s.w > 0 && s.h > 0 && pageNew()) {
    gDisplay->waveformLabEqualizeWindow(pageNew(), static_cast<uint16_t>(s.x),
                                        static_cast<uint16_t>(s.y), static_cast<uint16_t>(s.w),
                                        static_cast<uint16_t>(s.h));
  }
  bandPopFront();
}

void writeActiveWindowDiff() {
  if (gAnim.bandN <= 0 || !pageOld() || !pageNew()) return;
  int xMin = gAnim.band[0].x;
  int yMin = gAnim.band[0].y;
  int xMax = gAnim.band[0].x + gAnim.band[0].w;
  int yMax = gAnim.band[0].y + gAnim.band[0].h;
  for (int i = 1; i < gAnim.bandN; ++i) {
    const StepCell& c = gAnim.band[i];
    if (c.x < xMin) xMin = c.x;
    if (c.y < yMin) yMin = c.y;
    if (c.x + c.w > xMax) xMax = c.x + c.w;
    if (c.y + c.h > yMax) yMax = c.y + c.h;
  }
  const int w = xMax - xMin;
  const int h = yMax - yMin;
  if (w <= 0 || h <= 0) return;
  if ((xMin & 7) != 0 || (w & 7) != 0) return;
  gDisplay->waveformLabWriteDiffWindow(pageOld(), pageNew(), static_cast<uint16_t>(xMin),
                                       static_cast<uint16_t>(yMin), static_cast<uint16_t>(w),
                                       static_cast<uint16_t>(h));
}

bool pumpWindowStep() {
  if (gAnim.cur >= gAnim.steps && gAnim.bandN == 0) return false;
  if (gAnim.cur < gAnim.steps) {
    StepCell cell;
    stepCell(gAnim.cur, gAnim.steps, gAnim.dir, cell);
    ++gAnim.cur;
    if (cell.w > 0 && cell.h > 0 &&
        gAnim.bandN < static_cast<int>(sizeof(gAnim.band) / sizeof(gAnim.band[0]))) {
      gAnim.band[gAnim.bandN++] = cell;
    }
    while (gAnim.bandN > gAnim.widthMult) equalizeFrontCell();
    writeActiveWindowDiff();
    gDisplay->waveformLabActivate(gLut);
  } else {
    // Walk-off: window trailing edge finishes the screen. Re-drive remaining
    // band, then pop+equalize the front cell (no off-panel coordinates).
    writeActiveWindowDiff();
    gDisplay->waveformLabActivate(gLut);
    equalizeFrontCell();
  }
  return true;
}

void finishWindowAnim() {
  gDisplay->setCustomLUT(false, nullptr);
  // Stock equalize only — do not re-arm gLut for the seed write.
  if (pageNew()) gDisplay->waveformLabRefresh(pageNew(), pageNew(), /*lut=*/nullptr, /*turnOff=*/false);
  gAnim.active = false;
  gRunning = false;
  gAnim.pageOld = nullptr;
  gAnim.pageNew = nullptr;
  ++gRuns;
  gLastRunMs = millis() - gAnim.t0;
}

}  // namespace

bool startAnimateWindow(const char* prevPath, const char* nextPath, int steps, int winMult, int dir) {
  return startAnimate(prevPath, nextPath, steps, 0, /*windowMode=*/true, winMult, dir);
}

bool startAnimate(const char* prevPath, const char* nextPath, int steps, int feather, bool windowMode,
                  int winMult, int dir) {
  if (gRunning || gAnim.active) return false;
  if (!gLutSet) return false;
  if (steps < 1) steps = 1;
  if (steps > 64) steps = 64;
  if (feather < 0) feather = 0;
  if (feather > 64) feather = 64;
  if (winMult < 1) winMult = 1;
  if (winMult > steps) winMult = steps;
  if (dir < 0 || dir > 3) dir = 0;
  gAnim.widthMult = winMult;
  gAnim.dir = dir;
  if (!readFrameIntoSlot(prevPath, 0)) return false;
  if (!readFrameIntoSlot(nextPath, 1)) return false;
  gAnim.synth = nullptr;
  gAnim.bandN = 0;
  gAnim.pageOld = nullptr;
  gAnim.pageNew = nullptr;
  if (!windowMode) {
    gAnim.synth = allocSlot();
    if (!gAnim.synth) return false;
  }
  gAnim.active = true;
  gAnim.mode = windowMode ? 1 : 0;
  gAnim.steps = steps;
  gAnim.cur = 0;
  gAnim.feather = feather;
  gRunning = true;
  // Lab SD path may still baseline; reader mem path must not (see runAnimateMem*).
  if (windowMode) {
    // Keep the protocol's LUT state for diagnostics, but never arm it in the
    // display driver: custom LUTs can encode a multi-phase flashing waveform.
    gDisplay->setCustomLUT(false, nullptr);
  } else {
    gDisplay->waveformLabBaseline(gSlot[0]);
  }
  gAnim.t0 = millis();
  return true;
}

bool pumpAnimateWindow(uint32_t& stepMsOut) {
  if (!gAnim.active) return false;
  const uint32_t t0 = millis();

  if (gAnim.mode == 0) {
    if (gAnim.cur >= gAnim.steps) {
      gAnim.active = false;
      gRunning = false;
      if (gAnim.synth) {
        free(gAnim.synth);
        gAnim.synth = nullptr;
      }
      if (gDisplay) {
        gDisplay->setCustomLUT(false, nullptr);
        gDisplay->waveformLabRefresh(gSlot[1], gSlot[1], /*lut=*/nullptr, /*turnOff=*/false);
      }
      ++gRuns;
      gLastRunMs = millis() - gAnim.t0;
      stepMsOut = millis() - t0;
      return false;
    }
    if (!gAnim.synth) return false;
    ++gAnim.cur;
    const int edge = gAnim.W - (gAnim.W * gAnim.cur) / gAnim.steps;
    std::memset(gAnim.synth, 0, kFrameBytes);
    composeWipe(gSlot[0], gSlot[1], gAnim.synth, edge, gAnim.feather, gAnim.wb, gAnim.H);
    gDisplay->waveformLabRefresh(gSlot[0], gAnim.synth, gLut, /*turnOff=*/false);
    stepMsOut = millis() - t0;
    return true;
  }

  if (!pumpWindowStep()) {
    finishWindowAnim();
    stepMsOut = millis() - t0;
    return false;
  }
  stepMsOut = millis() - t0;
  return true;
}

bool animateActive() { return gAnim.active; }

uint32_t runAnimateWindow(const char* prevPath, const char* nextPath, int steps, uint32_t tailMs,
                          int winMult, int dir) {
  if (!startAnimateWindow(prevPath, nextPath, steps, winMult, dir)) return 0;
  uint32_t stepMs = 0;
  while (pumpAnimateWindow(stepMs)) {
    delay(2);
  }
  if (tailMs > 0 && tailMs <= 10000) {
    const uint8_t* o = gSlot[0];
    const uint8_t* n = gSlot[1];
    const uint32_t tailStart = millis();
    int tail = 0;
    while (tail < 12 && (millis() - tailStart) < tailMs) {
      gDisplay->waveformLabRefresh(o, n, gLut, /*turnOff=*/false);
      ++tail;
      delay(2);
    }
  }
  return gLastRunMs;
}

uint32_t runAnimateMemWindow(const uint8_t* oldFrame, const uint8_t* newFrame, int steps, int winMult,
                             int dir) {
  if (!gDisplay) return 0;
  // Never set gAnim.active here: main loop pumps pumpAnimateWindow() when
  // active, which races this blocking reader path and freezes after one turn.
  if (gRunning) gRunning = false;
  if (gAnim.active) {
    gAnim.active = false;
    gAnim.bandN = 0;
  }
  if (!gLutSet) return 0;
  if (!oldFrame || !newFrame) return 0;
  if (steps < 1) steps = 1;
  if (steps > 64) steps = 64;
  if (winMult < 1) winMult = 1;
  if (winMult > steps) winMult = steps;
  if (dir < 0 || dir > 3) dir = 0;

  // Self-contained blocking path (no async session / no gAnim.active).
  // Reader already shows oldFrame — no FULL baseline.
  StepCell band[64];
  int bandN = 0;
  gRunning = true;
  // This is the dedicated reader page-turn path: arm the custom LUT once for
  // the bounded differential window, then clear it before the stock final
  // same-frame seed. Generic UI never enters this path.
  gDisplay->setCustomLUT(true, gLut);
  const uint32_t t0 = millis();

  auto equalizeCell = [&](const StepCell& s) {
    if (s.w > 0 && s.h > 0) {
      gDisplay->waveformLabEqualizeWindow(newFrame, static_cast<uint16_t>(s.x),
                                          static_cast<uint16_t>(s.y), static_cast<uint16_t>(s.w),
                                          static_cast<uint16_t>(s.h));
    }
  };
  auto writeBand = [&]() {
    if (bandN <= 0) return;
    int xMin = band[0].x, yMin = band[0].y;
    int xMax = band[0].x + band[0].w, yMax = band[0].y + band[0].h;
    for (int i = 1; i < bandN; ++i) {
      if (band[i].x < xMin) xMin = band[i].x;
      if (band[i].y < yMin) yMin = band[i].y;
      if (band[i].x + band[i].w > xMax) xMax = band[i].x + band[i].w;
      if (band[i].y + band[i].h > yMax) yMax = band[i].y + band[i].h;
    }
    const int w = xMax - xMin, h = yMax - yMin;
    if (w <= 0 || h <= 0) return;
    if ((xMin & 7) != 0 || (w & 7) != 0) return;
    gDisplay->waveformLabWriteDiffWindow(oldFrame, newFrame, static_cast<uint16_t>(xMin),
                                         static_cast<uint16_t>(yMin), static_cast<uint16_t>(w),
                                         static_cast<uint16_t>(h));
  };

  // Temporarily publish geometry for stepCell().
  gAnim.W = 800;
  gAnim.H = 480;
  gAnim.wb = 100;

  auto popFront = [&]() {
    if (bandN <= 0) return;
    for (int j = 1; j < bandN; ++j) band[j - 1] = band[j];
    --bandN;
  };

  for (int i = 0; i < steps; ++i) {
    StepCell cell;
    stepCell(i, steps, dir, cell);
    if (cell.w > 0 && cell.h > 0 && bandN < 64) band[bandN++] = cell;
    while (bandN > winMult) {
      equalizeCell(band[0]);
      popFront();
    }
    writeBand();
    gDisplay->waveformLabActivate(gLut);
    delay(2);
  }
  // Walk-off drain: after the last strip enters, keep driving the remaining
  // mult-wide band and advance the trailing edge until empty. Every strip
  // (including the last) gets ~winMult activates — equivalent to the window
  // walking off-screen without writing off-panel geometry.
  while (bandN > 0) {
    writeBand();
    gDisplay->waveformLabActivate(gLut);
    equalizeCell(band[0]);
    popFront();
    delay(2);
  }

  gDisplay->setCustomLUT(false, nullptr);
  // Seed both planes with the *stock* FAST path (lut=nullptr). Passing gLut here
  // re-armed the experiment waveform for equalize and left RED/BW out of sync
  // for the next normal reader FAST → residual on local + plugin page turns.
  gDisplay->waveformLabRefresh(newFrame, newFrame, /*lut=*/nullptr, /*turnOff=*/false);

  gRunning = false;
  gAnim.active = false;
  gAnim.pageOld = nullptr;
  gAnim.pageNew = nullptr;
  const uint32_t total = millis() - t0;
  ++gRuns;
  gLastRunMs = total;
  return total;
}

uint32_t runSettle(const char* prevPath, const char* nextPath) {
  if (!gDisplay || gRunning) return 0;
  if (!gLutSet) return 0;
  if (!readFrameIntoSlot(prevPath, 0)) return 0;
  if (!readFrameIntoSlot(nextPath, 1)) return 0;
  gRunning = true;
  const uint32_t t0 = millis();
  // Full-frame differential: RED=old, BW=new -> one policy-guarded FAST pass.
  gDisplay->waveformLabRefresh(gSlot[0], gSlot[1], gLut, /*turnOff=*/false);
  gRunning = false;
  const uint32_t total = millis() - t0;
  ++gRuns;
  gLastRunMs = total;
  return total;
}

uint32_t runRefresh(bool swapAfter) {  if (!gDisplay || gRunning) return 0;
  if (!gLutSet) return 0;
  if (gSdFramesSet) {
    if (!readFrameIntoSlot(gSdPrev, 0) || !readFrameIntoSlot(gSdNext, 1)) return 0;
  } else {
    if (!gSlot[0] || !gSlot[1]) return 0;
    if (!frameUploadComplete(0) || !frameUploadComplete(1)) return 0;
  }
  gRunning = true;
  // prev = slot0 (old), next = slot1 (new).
  gLastRunMs = gDisplay->waveformLabRefresh(gSlot[0], gSlot[1], gLut, /*turnOff=*/false);
  if (swapAfter) {
    std::swap(gSlot[0], gSlot[1]);
    gSlotReceived[0] = gSlotReceived[1] = kFrameBytes;
  }
  gRunning = false;
  ++gRuns;
  // Diagnostics: append one line to /waveform/lab_diag.txt (SD), so the host
  // can read the actual refresh path taken without racing the log stream.
  {
    FsFile f;
    if (SDCardManager::getInstance().openFileForWrite("LAB", "/waveform/lab_diag.txt", f)) {
      char line[96];
      snprintf(line, sizeof(line), "run=%u ms=%u lut_set=%d\n", static_cast<unsigned>(gRuns),
               static_cast<unsigned>(gLastRunMs), gLutSet ? 1 : 0);
      f.write(line, strlen(line));
      f.close();
    }
  }
  return gLastRunMs;
}

void clearAll() {
  if (!gDisplay) return;
  if (gRunning) return;
  // Safe recovery: fast refresh of the current framebuffer content via the
  // standard policy path, then drop all experiment state.
  gRunning = true;
  gDisplay->refreshDisplay(HalDisplay::FAST_REFRESH);
  gRunning = false;
  gLutSet = false;
  gUploadTarget = false;
  gSdFramesSet = false;
  gRuns = 0;
  gLastRunMs = 0;
}

Stats stats() {
  Stats s;
  s.lastRunMs = gLastRunMs;
  s.runs = gRuns;
  s.lutSet = gLutSet;
  s.active = gLutSet;
  s.framesReady = frameUploadComplete(0) && frameUploadComplete(1);
  return s;
}

}  // namespace M4WaveformLab
