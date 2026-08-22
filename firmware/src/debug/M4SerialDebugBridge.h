#pragma once

// USB CDC serial debug bridge for Murphy M4 automated testing (m4adb).
// Compiled into Murphy M4 firmware only (CROSSPOINT_MURPHY_M4). Runtime-gated by
// Developer Options → USB serial debugging (default off).

#if defined(CROSSPOINT_MURPHY_M4)

#include "debug/M4SerialDebugPolicy.h"
#include "debug/M4SynthInputGate.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

class GfxRenderer;
class MappedInputManager;
class HalDisplay;

namespace M4SerialDebug {

inline constexpr const char* kPrefix = "@M4DBG/1";
inline constexpr int kProtocolVersion = 1;
inline constexpr size_t kMaxLineLen = M4SerialDebugPolicy::kMaxLineLen;
inline constexpr size_t kMaxRawChunk = M4SerialDebugPolicy::kMaxRawChunk;
inline constexpr size_t kMaxPackageBytes = M4SerialDebugPolicy::kMaxPackageBytes;
inline constexpr size_t kMaxReqIdLen = M4SerialDebugPolicy::kMaxReqIdLen;
inline constexpr size_t kIdempotencySlots = 12;
inline constexpr size_t kMaxJsonDecode = 1024;

struct StatusSnapshot {
  // Pointers must remain valid until replyOk finishes (copy into response).
  const char* firmwareVersion = "";
  const char* activity = "";
  const char* activeAppId = "";
  uint32_t freeHeap = 0;
  uint32_t minFreeHeap = 0;
  uint32_t freePsram = 0;
  uint32_t resetReason = 0;
  bool sdOk = false;
  int screenW = 0;
  int screenH = 0;
  int orientation = 0;
};

struct HostHooks {
  std::function<StatusSnapshot()> status;
  // Full UI text dump for automation (JSON object string). Prefer this over OCR.
  std::function<std::string()> uiDump;
  std::function<void()> goHome;
  // Open the native file-transfer activity.  The hook is intentionally
  // separate from goHome so a USB command can present the same UI as the
  // physical File Transfer entry without owning an Activity from the bridge.
  std::function<void()> openFileTransferUi;
  std::function<bool(const std::string& appId, std::string& errKey, std::string& errMsg)> launchApp;
  std::function<void(const std::string& appId)> noteActiveApp;
  std::function<void()> clearActiveApp;
  // Synchronous install on the main owner loop (no FreeRTOS worker).
  std::function<bool(const std::string& inboxPath, std::string& errKey, std::string& errMsg, std::string& id,
                     std::string& ver, int& code)>
      installSync;
};

class Bridge {
 public:
  void begin(GfxRenderer* renderer, MappedInputManager* input, HalDisplay* display, HostHooks hooks);
  // Owner main loop only. desiredAuthorized comes from SETTINGS (local UI only).
  // Transitions are idempotent; enable flushes stale RX, disable aborts work.
  void setAuthorized(bool desiredAuthorized);
  bool isAuthorized() const { return auth_.authorized; }

  // Call from main loop after beginFrame(), before activity->loop().
  void poll();

  // Mark poll() as a yield-reentry (m4YieldToDebugBridge from inside an
  // activity frame). Synthetic tap/swipe/key received in this context are
  // queued (ACK still immediate) and injected by a later regular poll(),
  // which runs in the beginFrame() input window — at most one event per
  // normal frame; the queue head is retained while the input manager reports
  // transient busy/rate-limit. Without this the one-frame synthetic event is
  // cleared before any activity sees it.
  void setYieldContext(bool on) { yieldContext_ = on; }

  // Recent host frame activity — used to prevent auto-sleep during scripted sessions.
  // Never keeps awake when unauthorized.
  bool recentHostActivity(unsigned long nowMs,
                          unsigned long windowMs = M4SerialDebugPolicy::kHostActivityWindowMs) const;
  void noteHostActivity();

 private:
  GfxRenderer* renderer_ = nullptr;
  MappedInputManager* input_ = nullptr;
  HostHooks hooks_;

  M4SerialDebugPolicy::AuthorizationState auth_;
  M4SerialDebugPolicy::LineIntake intake_;
  M4SerialDebugPolicy::ChunkSession chunk_;

  char uploadName_[80] = {};
  char uploadShaHex_[65] = {};
  void* uploadFile_ = nullptr;
  alignas(8) uint8_t shaCtx_[256] = {};
  bool shaReady_ = false;
  bool uploadActive_ = false;

  // Last chunk ack payload for replay (JSON object, small).
  char lastChunkAckJson_[96] = {};

  bool shotActive_ = false;
  char shotReqId_[kMaxReqIdLen + 1] = {};
  uint32_t shotOffset_ = 0;
  uint32_t shotTotal_ = 0;

  // Waveform Lab frame upload: chunks land in a PSRAM slot instead of SD.
  bool labFrameActive_ = false;
  int labFrameSlot_ = 0;

  struct IdemSlot {
    char id[kMaxReqIdLen + 1] = {};
    char kind[8] = {};
    char payloadB64[512] = {};
    bool used = false;
  };
  IdemSlot idem_[kIdempotencySlots] = {};
  size_t idemNext_ = 0;

  unsigned long lastHostActivityMs_ = 0;
  // Off→on authorization is not executable until every byte queued while off
  // has been discarded. Draining is bounded per loop but continues to empty.
  bool enableRxDrainPending_ = false;
  bool inPoll_ = false;

  // Deferred synthetic input received mid-frame (yield context). Bounded;
  // overflow rejects with busy so the host retries.
  static constexpr size_t kMaxDeferredInputs = 6;
  M4SynthInputGate::Gate<kMaxDeferredInputs> deferredInputs_;
  bool yieldContext_ = false;

  // Stable copies for status snprintf (Activity::getName() is stable while activity lives;
  // we still copy so StatusSnapshot pointers never dangle if Activity switches mid-reply).
  char activityCopy_[48] = {};
  char appIdCopy_[72] = {};

  void feedByte(char c);
  void handleLine(const char* line);
  bool parseFrame(const char* line, char* reqIdOut, size_t reqIdCap, char* kindOut, size_t kindCap,
                  const char** payloadStart);
  void handleReq(const char* reqId, const char* json, size_t jsonLen);
  void handleChk(const char* reqId, uint32_t seq, uint32_t total, const uint8_t* data, size_t len);
  void replyOk(const char* reqId, const char* jsonObj, bool cacheIdem = false);
  void replyErr(const char* reqId, const char* key, const char* zhMsg, bool cacheIdem = false);
  // Non-terminal progress for long ops (install_http). Same req_id; host keeps waiting for ok/err.
  void replyProgress(const char* reqId, const char* jsonObj);
  bool tryIdemReplay(const char* reqId);
  void storeIdem(const char* reqId, const char* kind, const char* payloadB64);

  bool beginUpload(const char* reqId, const char* name, uint32_t size, const char* shaHex);
  void abortUpload(bool removePart);
  void finishUploadAndInstall(const char* reqId);
  // Wi-Fi bulk path: device HTTP-GET url → inbox → SHA → installSync.
  // Serial carries only the control frame (no package bytes).
  void installFromHttpUrl(const char* reqId, const char* name, uint32_t size, const char* shaHex,
                          const char* url);
  bool hashFileSha256Hex(const char* path, char outHex[65]);
  // Shared post-stage path used by serial commit and install_http.
  void installStagedInboxPackage(const char* reqId, const char* name, const char* shaHex,
                                 const char* finalPath, const char* partPath);

  void beginScreenshot(const char* reqId);
  void pollScreenshot();
  bool logicalPixelBlack(int x, int y) const;
  void resetSessionState(bool removePart);
  void flushRxDiscard();
  void clearIdempotency();
  // Deliver at most one deferred synthetic input per regular poll window.
  // Retains the head while the input manager reports transient busy/rate-limit.
  void deliverDeferredInput();

  static bool hexEqSha256(const char* hex64, const uint8_t digest[32]);
};

}  // namespace M4SerialDebug

#endif  // CROSSPOINT_MURPHY_M4
