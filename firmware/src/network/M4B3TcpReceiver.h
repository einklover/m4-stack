#pragma once

// Production ESP32-S3 M4B3 TCP listener on 48624 (STA Wi-Fi only).
// One client. Persistent 480x800 MONO1 logical framebuffer in PSRAM.
// FRAME_ACK remains protocol/CRC accept only. Panel present is adjacent.

#if defined(CROSSPOINT_MURPHY_M4)

#include <cstddef>
#include <cstdint>

#include "util/M4B3Protocol.h"

namespace M4B3Tcp {

struct Snapshot {
  bool listening = false;
  bool connected = false;
  bool helloOk = false;
  char peer[24] = {};
  char bindIp[16] = {};
  int32_t acceptedFrameId = -1;
  uint32_t acceptedCrc = 0;
  uint32_t keys = 0;
  uint32_t patches = 0;
  uint32_t nacks = 0;
  uint32_t hellos = 0;
  uint32_t pings = 0;
  uint32_t bytesRx = 0;
  uint32_t bytesTx = 0;
  uint32_t applyErrors = 0;
  uint32_t reconnects = 0;
  uint8_t lastNack = 0xFF;
  uint32_t freeHeap = 0;
  uint32_t minFreeHeap = 0;
  uint32_t freePsram = 0;
  uint32_t rxFilled = 0;
  uint8_t panelOwner = 0;
  bool panelBusy = false;
  bool panelPending = false;
  uint32_t presentReq = 0;
  uint32_t presentOk = 0;
  uint32_t presentCoal = 0;
  uint32_t presentDrop = 0;
  uint32_t presentErr = 0;
  uint32_t mapErr = 0;
  int32_t panelSrcId = -1;
  uint32_t panelSrcCrc = 0;
  uint32_t panelCrc = 0;
  uint32_t panelLastErr = 0;
  uint32_t panelAgeMs = 0;
  uint8_t panelCorner[4] = {};
};

void begin();
void snapshot(Snapshot& out);

}  // namespace M4B3Tcp

#endif
