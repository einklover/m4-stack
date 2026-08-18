#pragma once

// ESP32 glue: map an accepted M4B3 logical frame into a PSRAM panel snapshot
// and present it with HalDisplay full or production-safe window Fast/DU
// refresh on the main owner loop. Does not change FRAME_ACK semantics.

#if defined(CROSSPOINT_MURPHY_M4)

#include <cstddef>
#include <cstdint>

class HalDisplay;

namespace M4B3Panel {

struct Snapshot {
  uint8_t owner = 0;
  bool busy = false;
  bool pending = false;
  bool baselineTrusted = false;
  bool everPresented = false;
  uint32_t baselineEpoch = 0;
  uint32_t requested = 0;
  uint32_t completed = 0;
  uint32_t coalesced = 0;
  uint32_t dropped = 0;
  uint32_t presentErrors = 0;
  uint32_t mapErrors = 0;
  uint32_t fullReq = 0;
  uint32_t fullOk = 0;
  uint32_t fullErr = 0;
  uint32_t partialReq = 0;
  uint32_t partialOk = 0;
  uint32_t partialErr = 0;
  uint32_t noChange = 0;
  uint32_t partialsSinceFull = 0;
  uint32_t cumulativePartialPixels = 0;
  uint32_t lastDirtyPixels = 0;
  uint32_t lastDirtyArea = 0;
  uint16_t lastRectCount = 0;
  uint32_t lastPolicyReason = 0;
  uint32_t lastFullMs = 0;
  uint32_t lastPartialMs = 0;
  int32_t sourceFrameId = -1;
  uint32_t sourceCrc = 0;
  uint32_t panelCrc = 0;
  uint32_t lastError = 0;
  uint32_t lastRequestMs = 0;
  uint32_t lastCompleteMs = 0;
  uint32_t ageMs = 0;
  uint8_t corner[4] = {};
  uint16_t lastWin[4][4] = {};  // up to 4 windows: x,y,w,h
};

bool begin();
void offerAccepted(const uint8_t* logical, size_t logicalLen, int32_t frameId, uint32_t sourceCrc);
void noteDisconnect();
void invalidatePhysicalBaseline();
void notePanelReinit();
void tick(HalDisplay& display, uint32_t nowMs);
bool browserOwnsDisplay();
void snapshot(Snapshot& out, uint32_t nowMs = 0);
void injectNextFailure();

}  // namespace M4B3Panel

#endif
