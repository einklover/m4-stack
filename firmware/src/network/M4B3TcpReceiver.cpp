#if defined(CROSSPOINT_MURPHY_M4)

#include "network/M4B3TcpReceiver.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include <atomic>
#include <cstring>
#include <new>

#include <HalGPIO.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

#include "apps/providers/M4Psram.h"
#include "CrossPointSettings.h"
#include "network/M4B3DiscoveryAdvertise.h"
#include "network/M4B3Panel.h"
#include "util/M4B3Input.h"
#include "util/M4B3Key.h"
#include "util/M4PanelMapper.h"

// Compile the mapper into the production firmware without touching the
// 480x800 transport framebuffer or the panel/waveform path.
static_assert(M4PanelMapper::kLogicalWidth == M4B3::kWidth, "logical width");
static_assert(M4PanelMapper::kLogicalHeight == M4B3::kHeight, "logical height");
static_assert(M4PanelMapper::kLogicalStride == M4B3::kStride, "logical stride");
static_assert(M4PanelMapper::kLogicalSize == M4B3::kKeyframeSize, "logical size");
static_assert(M4PanelMapper::kPhysicalWidth == 800 && M4PanelMapper::kPhysicalHeight == 480,
              "panel-native 800x480");
static_assert(M4PanelMapper::kPhysicalSize == 48000u, "panel buffer 48,000 B");

namespace M4B3Tcp {
namespace {

constexpr uint16_t kPort = 48624;
constexpr TickType_t kPollTicks = pdMS_TO_TICKS(20);
constexpr uint32_t kDiagEveryMs = 5000;

struct Runtime {
  M4B3::Session session;
  M4B3::StreamParser parser;
  uint8_t* accepted = nullptr;
  uint8_t* candidate = nullptr;
  uint8_t* rx = nullptr;
  uint8_t tx[64] = {};
  uint8_t touchTx[M4B3::kEnvelopeSize + M4B3::kTouchHeaderSize] = {};
  uint8_t keyTx[M4B3::kEnvelopeSize + M4B3::kInputKeyHeaderSize] = {};
  WiFiServer* server = nullptr;
  WiFiClient client;
  TaskHandle_t task = nullptr;
  std::atomic<bool> started{false};
  std::atomic<bool> listening{false};
  std::atomic<bool> connected{false};
  std::atomic<uint32_t> reconnects{0};
  char peer[24] = {};
  char bindIp[16] = {};
  uint32_t lastDiagMs = 0;
  portMUX_TYPE inputMux = portMUX_INITIALIZER_UNLOCKED;
  M4B3Input::Queue input;
  M4B3Key::Queue keys;
  uint32_t touchEnvSeq = 0;
  uint32_t touchTxErr = 0;
  uint32_t touchLastLatencyMs = 0;
  uint32_t keyEnvSeq = 0;
  uint32_t keyTxErr = 0;
  uint32_t keyLastLatencyMs = 0;
  int lastPanelX = 0;
  int lastPanelY = 0;
  bool haveLastPanel = false;
};

Runtime gRt;

void copyCapped(char* dst, size_t cap, const char* src) {
  if (!dst || cap == 0) return;
  size_t n = 0;
  if (src) {
    while (src[n] && n + 1 < cap) {
      dst[n] = src[n];
      ++n;
    }
  }
  dst[n] = 0;
}

void observeHeap(Snapshot& s) {
  s.freeHeap = ESP.getFreeHeap();
  s.minFreeHeap = ESP.getMinFreeHeap();
  s.freePsram = ESP.getFreePsram();
}

void fillSnapshot(Snapshot& s) {
  s.listening = gRt.listening.load(std::memory_order_relaxed);
  s.connected = gRt.connected.load(std::memory_order_relaxed);
  s.helloOk = gRt.session.helloOk();
  copyCapped(s.peer, sizeof(s.peer), gRt.peer);
  copyCapped(s.bindIp, sizeof(s.bindIp), gRt.bindIp);
  s.mdnsAdvertised = M4B3DiscoveryAdvertise::advertised();
  s.mdnsAdds = M4B3DiscoveryAdvertise::adds();
  s.mdnsRemoves = M4B3DiscoveryAdvertise::removes();
  s.mdnsErrors = M4B3DiscoveryAdvertise::errors();
  s.acceptedFrameId = gRt.session.acceptedFrameId();
  s.acceptedCrc = gRt.session.acceptedCrc();
  const M4B3::Stats& st = gRt.session.stats();
  s.keys = st.keys;
  s.patches = st.patches;
  s.nacks = st.nacks;
  s.hellos = st.hellos;
  s.pings = st.pings;
  s.bytesRx = st.bytesRx;
  s.bytesTx = st.bytesTx;
  s.applyErrors = st.applyErrors;
  s.reconnects = gRt.reconnects.load(std::memory_order_relaxed);
  s.lastNack = st.lastNack;
  s.rxFilled = static_cast<uint32_t>(gRt.parser.filled);
  observeHeap(s);
  M4B3Panel::Snapshot panel;
  M4B3Panel::snapshot(panel);
  s.panelOwner = panel.owner;
  s.panelBusy = panel.busy;
  s.panelPending = panel.pending;
  s.presentReq = panel.requested;
  s.presentOk = panel.completed;
  s.presentCoal = panel.coalesced;
  s.presentDrop = panel.dropped;
  s.presentErr = panel.presentErrors;
  s.mapErr = panel.mapErrors;
  s.panelSrcId = panel.sourceFrameId;
  s.panelSrcCrc = panel.sourceCrc;
  s.panelCrc = panel.panelCrc;
  s.panelLastErr = panel.lastError;
  s.panelAgeMs = panel.ageMs;
  s.panelCorner[0] = panel.corner[0];
  s.panelCorner[1] = panel.corner[1];
  s.panelCorner[2] = panel.corner[2];
  s.panelCorner[3] = panel.corner[3];
  s.panelTrusted = panel.baselineTrusted;
  s.panelEverPresented = panel.everPresented;
  s.panelEpoch = panel.baselineEpoch;
  s.fullReq = panel.fullReq;
  s.fullOk = panel.fullOk;
  s.fullErr = panel.fullErr;
  s.partialReq = panel.partialReq;
  s.partialOk = panel.partialOk;
  s.partialErr = panel.partialErr;
  s.hygieneReq = panel.hygieneReq;
  s.hygieneOk = panel.hygieneOk;
  s.hygieneErr = panel.hygieneErr;
  s.noChange = panel.noChange;
  s.partialsSinceFull = panel.partialsSinceFull;
  s.cumulativePartialPixels = panel.cumulativePartialPixels;
  s.uniqueCoveragePixels = panel.uniqueCoveragePixels;
  s.lastDirtyPixels = panel.lastDirtyPixels;
  s.lastDirtyArea = panel.lastDirtyArea;
  s.lastRectCount = panel.lastRectCount;
  s.lastPolicyReason = panel.lastPolicyReason;
  s.lastFullMs = panel.lastFullMs;
  s.lastPartialMs = panel.lastPartialMs;
  s.lastHygieneMs = panel.lastHygieneMs;
  for (int i = 0; i < 4; ++i) {
    s.lastWin[i][0] = panel.lastWin[i][0];
    s.lastWin[i][1] = panel.lastWin[i][1];
    s.lastWin[i][2] = panel.lastWin[i][2];
    s.lastWin[i][3] = panel.lastWin[i][3];
  }
  portENTER_CRITICAL(&gRt.inputMux);
  const M4B3Input::Stats in = gRt.input.stats();
  s.touchDown = in.down;
  s.touchMove = in.move;
  s.touchUp = in.up;
  s.touchCancel = in.cancel;
  s.touchCoalesced = in.coalesced;
  s.touchDroppedMove = in.droppedMove;
  s.touchRejected = in.rejected;
  s.touchOverflow = in.overflow;
  s.touchSession = gRt.input.session();
  s.touchSessionResets = in.sessionResets;
  s.touchSessionCancels = in.sessionCancels;
  s.touchLastSeq = gRt.input.lastSeq();
  s.touchLastX = gRt.input.lastX();
  s.touchLastY = gRt.input.lastY();
  s.touchQueue = static_cast<uint8_t>(gRt.input.size());
  s.touchActive = gRt.input.active();
  portEXIT_CRITICAL(&gRt.inputMux);
  s.touchTxErr = gRt.touchTxErr;
  s.touchLastLatencyMs = gRt.touchLastLatencyMs;
  s.touchCapture = gRt.connected.load(std::memory_order_relaxed) && gRt.session.helloOk();
  portENTER_CRITICAL(&gRt.inputMux);
  const M4B3Key::Stats key = gRt.keys.stats();
  s.keyBack = key.back;
  s.keyReload = key.reload;
  s.keyRejected = key.rejected;
  s.keyOverflow = key.overflow;
  s.keySession = gRt.keys.session();
  s.keySessionResets = key.sessionResets;
  s.keyLastSeq = gRt.keys.lastSeq();
  s.keyLastAction = gRt.keys.lastAction();
  s.keyQueue = static_cast<uint8_t>(gRt.keys.size());
  portEXIT_CRITICAL(&gRt.inputMux);
  s.keyTxErr = gRt.keyTxErr;
  s.keyLastLatencyMs = gRt.keyLastLatencyMs;
  s.keyCapture = s.touchCapture;
}

void logSnapshot(const char* why) {
  Snapshot s;
  fillSnapshot(s);
  Serial.printf(
      "[%lu] [M4B3] %s listen=%d conn=%d peer=%s bind=%s hello=%d accepted=%ld crc=0x%08x "
      "key=%u patch=%u nack=%u helloN=%u ping=%u rx=%u tx=%u applyErr=%u recon=%u "
      "rxFill=%u heap=%u minHeap=%u psram=%u mdns=%d "
      "panel(owner=%u busy=%d pend=%d req=%u ok=%u coal=%u "
      "drop=%u src=%ld pcrc=0x%08x full=%u/%u part=%u/%u reason=%u "
      "touch(d=%u m=%u u=%u c=%u coal=%u dropM=%u rej=%u q=%u act=%d sess=%u) "
      "key(back=%u reload=%u rej=%u ovf=%u q=%u sess=%u txerr=%u)\n",
      millis(), why, s.listening ? 1 : 0, s.connected ? 1 : 0, s.peer[0] ? s.peer : "-",
      s.bindIp[0] ? s.bindIp : "-", s.helloOk ? 1 : 0, static_cast<long>(s.acceptedFrameId),
      static_cast<unsigned>(s.acceptedCrc), s.keys, s.patches, s.nacks, s.hellos, s.pings, s.bytesRx, s.bytesTx,
      s.applyErrors, s.reconnects, s.rxFilled, s.freeHeap, s.minFreeHeap, s.freePsram,
      s.mdnsAdvertised ? 1 : 0,
      static_cast<unsigned>(s.panelOwner), s.panelBusy ? 1 : 0, s.panelPending ? 1 : 0, s.presentReq,
      s.presentOk, s.presentCoal, s.presentDrop, static_cast<long>(s.panelSrcId),
      static_cast<unsigned>(s.panelCrc), s.fullOk, s.fullErr, s.partialOk, s.partialErr,
      s.lastPolicyReason, s.touchDown, s.touchMove, s.touchUp, s.touchCancel, s.touchCoalesced,
      s.touchDroppedMove, s.touchRejected, static_cast<unsigned>(s.touchQueue), s.touchActive ? 1 : 0,
      s.touchSession, s.keyBack, s.keyReload, s.keyRejected, s.keyOverflow,
      static_cast<unsigned>(s.keyQueue), s.keySession, s.keyTxErr);
}

void resetInputSession() {
  portENTER_CRITICAL(&gRt.inputMux);
  gRt.input.resetSession();
  gRt.keys.resetSession();
  gRt.haveLastPanel = false;
  portEXIT_CRITICAL(&gRt.inputMux);
}

void flushInput() {
  if (!gRt.client || !gRt.connected.load(std::memory_order_relaxed)) return;
  const uint32_t now = millis();
  for (;;) {
    M4B3Input::Event ev;
    portENTER_CRITICAL(&gRt.inputMux);
    const bool have = gRt.input.pop(ev);
    portEXIT_CRITICAL(&gRt.inputMux);
    // Touch and key are independent bounded queues. Exhausting touch must not
    // return from the function, otherwise the key queue below is unreachable.
    if (!have) break;
    const size_t n = M4B3::encodeTouch(gRt.touchTx, sizeof(gRt.touchTx), gRt.touchEnvSeq++, ev.action,
                                       ev.flags, ev.x, ev.y, ev.tMs, ev.seq, ev.session);
    if (n == 0) {
      gRt.touchTxErr++;
      continue;
    }
    const size_t wrote = gRt.client.write(gRt.touchTx, n);
    if (wrote != n) {
      gRt.touchTxErr++;
      return;
    }
    gRt.session.stats().bytesTx += static_cast<uint32_t>(n);
    gRt.touchLastLatencyMs = now >= ev.tMs ? (now - ev.tMs) : 0;
  }
  for (;;) {
    M4B3Key::Event ev;
    portENTER_CRITICAL(&gRt.inputMux);
    const bool have = gRt.keys.pop(ev);
    portEXIT_CRITICAL(&gRt.inputMux);
    if (!have) return;
    const size_t n = M4B3::encodeInputKey(gRt.keyTx, sizeof(gRt.keyTx), gRt.keyEnvSeq++, ev.action,
                                          ev.flags, ev.tMs, ev.seq, ev.session);
    if (n == 0) {
      gRt.keyTxErr++;
      continue;
    }
    const size_t wrote = gRt.client.write(gRt.keyTx, n);
    if (wrote != n) {
      gRt.keyTxErr++;
      return;
    }
    gRt.session.stats().bytesTx += static_cast<uint32_t>(n);
    gRt.keyLastLatencyMs = now >= ev.tMs ? (now - ev.tMs) : 0;
  }
}

bool staReady(IPAddress& ip) {
  if (WiFi.status() != WL_CONNECTED) return false;
  ip = WiFi.localIP();
  return ip != IPAddress(0, 0, 0, 0);
}

void closeClient(const char* why) {
  if (gRt.client) {
    gRt.client.stop();
  }
  const bool was = gRt.connected.exchange(false);
  gRt.parser.reset();
  gRt.peer[0] = 0;
  if (was) {
    resetInputSession();
  }
  M4B3Panel::noteDisconnect();
  if (was) {
    logSnapshot(why);
  }
}

void stopListen() {
  closeClient("wifi-down");
  M4B3DiscoveryAdvertise::sync(false, "");
  if (gRt.server) {
    gRt.server->stop();
    delete gRt.server;
    gRt.server = nullptr;
  }
  gRt.listening.store(false);
  gRt.bindIp[0] = 0;
}

bool startListen(const IPAddress& ip) {
  if (gRt.server) return true;
  auto* srv = new (std::nothrow) WiFiServer(kPort);
  if (!srv) return false;
  srv->begin();
  srv->setNoDelay(true);
  gRt.server = srv;
  copyCapped(gRt.bindIp, sizeof(gRt.bindIp), ip.toString().c_str());
  gRt.listening.store(true);
  M4B3DiscoveryAdvertise::sync(true, gRt.bindIp);
  Serial.printf("[%lu] [M4B3] listen %s:%u (STA only)\n", millis(), gRt.bindIp, static_cast<unsigned>(kPort));
  logSnapshot("listen");
  return true;
}

void handleComplete(const uint8_t* msg, size_t len) {
  const int32_t beforeId = gRt.session.acceptedFrameId();
  const uint32_t beforeCrc = gRt.session.acceptedCrc();
  const size_t n = gRt.session.handle(msg, len, gRt.tx, sizeof(gRt.tx));
  // FRAME_ACK means protocol/framebuffer accepted. Never wait for panel refresh.
  if (n > 0 && gRt.client) {
    gRt.client.write(gRt.tx, n);
  }
  const int32_t afterId = gRt.session.acceptedFrameId();
  const uint32_t afterCrc = gRt.session.acceptedCrc();
  if (afterId >= 0 && (afterId != beforeId || afterCrc != beforeCrc)) {
    M4B3Panel::offerAccepted(gRt.session.accepted(), M4B3::kKeyframeSize, afterId, afterCrc);
  }
}

void processRx() {
  while (true) {
    const uint8_t* msg = nullptr;
    size_t msgLen = 0;
    const M4B3::Status st = gRt.parser.nextMessage(&msg, &msgLen);
    if (st == M4B3::Status::Truncated) return;
    if (st != M4B3::Status::Ok) {
      gRt.session.stats().applyErrors++;
      Serial.printf("[%lu] [M4B3] stream error status=%u filled=%u — drop client\n", millis(),
                    static_cast<unsigned>(st), static_cast<unsigned>(gRt.parser.filled));
      closeClient("stream-error");
      return;
    }
    handleComplete(msg, msgLen);
    gRt.parser.consume(msgLen);
    // Input TX is adjacent and never gates FRAME_ACK / offerAccepted.
    flushInput();
  }
}

void serviceClient() {
  if (!gRt.client || !gRt.client.connected()) {
    if (gRt.connected.load()) closeClient("peer-close");
    return;
  }
  while (gRt.client.available() > 0) {
    const size_t room = (gRt.parser.cap > gRt.parser.filled) ? (gRt.parser.cap - gRt.parser.filled) : 0;
    if (room == 0) {
      Serial.printf("[%lu] [M4B3] rx buffer full — drop client\n", millis());
      closeClient("rx-full");
      return;
    }
    uint8_t tmp[1024];
    const size_t want = room < sizeof(tmp) ? room : sizeof(tmp);
    const int n = gRt.client.read(tmp, want);
    if (n <= 0) break;
    if (gRt.parser.append(tmp, static_cast<size_t>(n)) != M4B3::Status::Ok) {
      closeClient("append-fail");
      return;
    }
    processRx();
    if (!gRt.connected.load()) return;
  }
  flushInput();
}

void acceptIfIdle() {
  if (!gRt.server || gRt.connected.load()) return;
  WiFiClient incoming = gRt.server->accept();
  if (!incoming) return;
  if (gRt.client && gRt.client.connected()) {
    incoming.stop();
    return;
  }
  gRt.client = incoming;
  gRt.client.setNoDelay(true);
  gRt.client.setTimeout(20);
  gRt.parser.reset();
  copyCapped(gRt.peer, sizeof(gRt.peer), gRt.client.remoteIP().toString().c_str());
  gRt.connected.store(true);
  gRt.reconnects.fetch_add(1, std::memory_order_relaxed);
  logSnapshot("accept");
}

void maybeDiag() {
  const uint32_t now = millis();
  if (now - gRt.lastDiagMs < kDiagEveryMs) return;
  gRt.lastDiagMs = now;
  if (gRt.connected.load() || gRt.listening.load()) logSnapshot("diag");
}

void taskMain(void*) {
  Serial.printf("[%lu] [M4B3] receiver task start port=%u fb=%u parse=%u\n", millis(), static_cast<unsigned>(kPort),
                static_cast<unsigned>(M4B3::kKeyframeSize), static_cast<unsigned>(M4B3::kMaxMessageSize));
  for (;;) {
    IPAddress ip;
    if (!staReady(ip)) {
      if (gRt.listening.load()) stopListen();
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    if (!gRt.listening.load()) {
      if (!startListen(ip)) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
    }
    M4B3DiscoveryAdvertise::sync(true, gRt.bindIp);
    acceptIfIdle();
    serviceClient();
    flushInput();
    maybeDiag();
    vTaskDelay(kPollTicks);
  }
}

bool allocBuffers() {
  if (gRt.accepted) return true;
  gRt.accepted = static_cast<uint8_t*>(M4Psram::mallocPrefer(M4B3::kKeyframeSize));
  gRt.candidate = static_cast<uint8_t*>(M4Psram::mallocPrefer(M4B3::kKeyframeSize));
  gRt.rx = static_cast<uint8_t*>(M4Psram::mallocPrefer(M4B3::kMaxMessageSize));
  if (!gRt.accepted || !gRt.candidate || !gRt.rx) {
    Serial.printf("[%lu] [M4B3] PSRAM alloc failed accepted=%p candidate=%p rx=%p\n", millis(),
                  static_cast<void*>(gRt.accepted), static_cast<void*>(gRt.candidate), static_cast<void*>(gRt.rx));
    return false;
  }
  std::memset(gRt.accepted, 0xFF, M4B3::kKeyframeSize);
  std::memset(gRt.candidate, 0xFF, M4B3::kKeyframeSize);
  std::memset(gRt.rx, 0, M4B3::kMaxMessageSize);
  gRt.session.attach(gRt.accepted, gRt.candidate);
  gRt.parser.attach(gRt.rx, M4B3::kMaxMessageSize);
  Serial.printf("[%lu] [M4B3] buffers PSRAM accepted=%u candidate=%u parse=%u\n", millis(),
                static_cast<unsigned>(M4B3::kKeyframeSize), static_cast<unsigned>(M4B3::kKeyframeSize),
                static_cast<unsigned>(M4B3::kMaxMessageSize));
  return true;
}

}  // namespace

void begin() {
  bool expected = false;
  if (!gRt.started.compare_exchange_strong(expected, true)) return;
  if (!allocBuffers()) {
    gRt.started.store(false);
    return;
  }
  if (!M4B3Panel::begin()) {
    Serial.printf("[%lu] [M4B3] panel presenter alloc failed — transport continues without panel\n",
                  millis());
  }
  TaskHandle_t h = nullptr;
  if (M4Psram::createTask(taskMain, "M4B3Rx", 12u * 1024u, nullptr, 1, &h) != pdPASS) {
    Serial.printf("[%lu] [M4B3] createTask failed\n", millis());
    gRt.started.store(false);
    return;
  }
  gRt.task = h;
}

void snapshot(Snapshot& out) { fillSnapshot(out); }

bool inputCaptureActive() {
  return gRt.connected.load(std::memory_order_relaxed) && gRt.session.helloOk();
}

void captureFromGpio(HalGPIO& gpio, uint32_t nowMs) {
  if (!inputCaptureActive()) return;

  // Browser hardware keys do not depend on touch state. Capture them first so
  // a fresh session with no prior panel coordinate cannot drop Back/Confirm.
  // Emit on release so one physical click produces exactly one Browser action.
  const uint8_t backHw = SETTINGS.frontButtonBack;
  const uint8_t reloadHw = SETTINGS.frontButtonConfirm;
  portENTER_CRITICAL(&gRt.inputMux);
  if (gpio.wasReleased(backHw)) {
    (void)gRt.keys.push(M4B3::kInputKeyBack, nowMs);
  }
  if (reloadHw != backHw && gpio.wasReleased(reloadHw)) {
    (void)gRt.keys.push(M4B3::kInputKeyReload, nowMs);
  }
  portEXIT_CRITICAL(&gRt.inputMux);

  int px = 0;
  int py = 0;
  if (gpio.getTouchPanelPoint(px, py)) {
    gRt.lastPanelX = px;
    gRt.lastPanelY = py;
    gRt.haveLastPanel = true;
  }
  if (!gRt.haveLastPanel && !gpio.wasTouchPressed() && !gpio.wasTouchReleased()) return;

  int lx = 0;
  int ly = 0;
  if (!M4B3Input::panelToLogical(gRt.lastPanelX, gRt.lastPanelY, &lx, &ly)) {
    portENTER_CRITICAL(&gRt.inputMux);
    gRt.input.stats().rejected++;
    portEXIT_CRITICAL(&gRt.inputMux);
    return;
  }
  const uint16_t x = static_cast<uint16_t>(lx);
  const uint16_t y = static_cast<uint16_t>(ly);
  portENTER_CRITICAL(&gRt.inputMux);
  if (gpio.wasTouchPressed()) {
    (void)gRt.input.push(M4B3::kTouchDown, x, y, nowMs);
  } else if (gpio.isTouchPressed()) {
    (void)gRt.input.push(M4B3::kTouchMove, x, y, nowMs);
  }
  if (gpio.wasTouchReleased()) {
    (void)gRt.input.push(M4B3::kTouchUp, x, y, nowMs);
  }
  portEXIT_CRITICAL(&gRt.inputMux);
}

}  // namespace M4B3Tcp

#endif
