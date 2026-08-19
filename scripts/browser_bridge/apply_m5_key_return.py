#!/usr/bin/env python3
"""Apply the ChatGPT-authored M5 Browser Bridge hardware-key integration.

This is intentionally mechanical. It performs exact, one-shot replacements
against the validated #39 integration head plus the M5 helper/test files.
Do not reinterpret the architecture while applying it.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_once(rel: str, old: str, new: str) -> None:
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{rel}: expected one match, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")
    print(f"updated {rel}")


# ---------------------------------------------------------------------------
# Firmware protocol: additive M4 -> Android INPUT_KEY type 8.
# ---------------------------------------------------------------------------
replace_once(
    "firmware/src/util/M4B3Protocol.h",
    """constexpr uint8_t kTypePong = 6;\nconstexpr uint8_t kTypeTouch = 7;\n""",
    """constexpr uint8_t kTypePong = 6;\nconstexpr uint8_t kTypeTouch = 7;\nconstexpr uint8_t kTypeInputKey = 8;\n""",
)
replace_once(
    "firmware/src/util/M4B3Protocol.h",
    """constexpr uint16_t kPingHeaderSize = 4;\nconstexpr uint16_t kTouchHeaderSize = 20;\nconstexpr uint16_t kRectMetaSize = 12;\n""",
    """constexpr uint16_t kPingHeaderSize = 4;\nconstexpr uint16_t kTouchHeaderSize = 20;\nconstexpr uint16_t kInputKeyHeaderSize = 16;\nconstexpr uint16_t kRectMetaSize = 12;\n""",
)
replace_once(
    "firmware/src/util/M4B3Protocol.h",
    """inline bool validTouchAction(uint8_t action) {\n  return action >= kTouchDown && action <= kTouchCancel;\n}\n\nconstexpr uint16_t kMaxHeaderLen = 32;\n""",
    """inline bool validTouchAction(uint8_t action) {\n  return action >= kTouchDown && action <= kTouchCancel;\n}\n\n// Browser-local hardware key actions. These never mean Android-global input.\nconstexpr uint8_t kInputKeyBack = 1;\nconstexpr uint8_t kInputKeyReload = 2;\n\ninline bool validInputKeyAction(uint8_t action) {\n  return action == kInputKeyBack || action == kInputKeyReload;\n}\n\nconstexpr uint16_t kMaxHeaderLen = 32;\n""",
)
replace_once(
    "firmware/src/util/M4B3Protocol.h",
    """inline bool isKnownType(uint8_t type) { return type >= kTypeHello && type <= kTypeTouch; }\n""",
    """inline bool isKnownType(uint8_t type) { return type >= kTypeHello && type <= kTypeInputKey; }\n""",
)
replace_once(
    "firmware/src/util/M4B3Protocol.h",
    """inline size_t encodeTouch(uint8_t* out, size_t cap, uint32_t envSeq, uint8_t action, uint8_t flags,\n                          uint16_t x, uint16_t y, uint32_t tMs, uint32_t inputSeq, uint32_t session) {\n  uint8_t h[kTouchHeaderSize];\n  h[0] = action;\n  h[1] = flags;\n  wr16(h + 2, 0);\n  wr16(h + 4, x);\n  wr16(h + 6, y);\n  wr32(h + 8, tMs);\n  wr32(h + 12, inputSeq);\n  wr32(h + 16, session);\n  return wrap(out, cap, kTypeTouch, 0, envSeq, h, kTouchHeaderSize, nullptr, 0);\n}\n\ninline size_t encodeKeyframe""",
    """inline size_t encodeTouch(uint8_t* out, size_t cap, uint32_t envSeq, uint8_t action, uint8_t flags,\n                          uint16_t x, uint16_t y, uint32_t tMs, uint32_t inputSeq, uint32_t session) {\n  uint8_t h[kTouchHeaderSize];\n  h[0] = action;\n  h[1] = flags;\n  wr16(h + 2, 0);\n  wr16(h + 4, x);\n  wr16(h + 6, y);\n  wr32(h + 8, tMs);\n  wr32(h + 12, inputSeq);\n  wr32(h + 16, session);\n  return wrap(out, cap, kTypeTouch, 0, envSeq, h, kTouchHeaderSize, nullptr, 0);\n}\n\n// INPUT_KEY header (16 B LE): action u8, flags u8, reserved u16,\n// t_ms u32, input_seq u32, session u32. Payload is empty.\ninline size_t encodeInputKey(uint8_t* out, size_t cap, uint32_t envSeq, uint8_t action, uint8_t flags,\n                             uint32_t tMs, uint32_t inputSeq, uint32_t session) {\n  if (!validInputKeyAction(action)) return 0;\n  uint8_t h[kInputKeyHeaderSize];\n  h[0] = action;\n  h[1] = flags;\n  wr16(h + 2, 0);\n  wr32(h + 4, tMs);\n  wr32(h + 8, inputSeq);\n  wr32(h + 12, session);\n  return wrap(out, cap, kTypeInputKey, 0, envSeq, h, kInputKeyHeaderSize, nullptr, 0);\n}\n\ninline size_t encodeKeyframe""",
)
replace_once(
    "firmware/src/util/M4B3Protocol.h",
    """      case kTypePong:\n      case kTypeFrameAck:\n      case kTypeTouch:\n        return 0;\n""",
    """      case kTypePong:\n      case kTypeFrameAck:\n      case kTypeTouch:\n      case kTypeInputKey:\n        return 0;\n""",
)

# ---------------------------------------------------------------------------
# Local input ownership: physical Back/Confirm are Browser-owned while hello-ok.
# Synthetic/debug Back remains available to local firmware deliberately.
# ---------------------------------------------------------------------------
replace_once(
    "firmware/src/MappedInputManager.h",
    """  void setTouchRoutedToBrowser(bool routed) { touchRoutedToBrowser_ = routed; }\n  bool touchRoutedToBrowser() const { return touchRoutedToBrowser_; }\n""",
    """  void setTouchRoutedToBrowser(bool routed) { touchRoutedToBrowser_ = routed; }\n  bool touchRoutedToBrowser() const { return touchRoutedToBrowser_; }\n  // Browser Bridge also owns the logical Back + Confirm hardware controls while\n  // its M4B3 session is hello-ok. This prevents the hidden local Activity from\n  // reacting to the same physical key that is returned to Android.\n  void setKeysRoutedToBrowser(bool routed) { keysRoutedToBrowser_ = routed; }\n  bool keysRoutedToBrowser() const { return keysRoutedToBrowser_; }\n""",
)
replace_once(
    "firmware/src/MappedInputManager.h",
    """  const GfxRenderer* renderer = nullptr;\n  bool touchRoutedToBrowser_ = false;\n""",
    """  const GfxRenderer* renderer = nullptr;\n  bool touchRoutedToBrowser_ = false;\n  bool keysRoutedToBrowser_ = false;\n""",
)
replace_once(
    "firmware/src/MappedInputManager.cpp",
    """bool MappedInputManager::wasPressed(const Button button) const {\n  if (button == Button::Back && syntheticBack) return true;\n#if defined(CROSSPOINT_MURPHY_M4)\n  if (synthKind_ == SynthKind::Key && synthKey_ == button) return true;\n#endif\n  return mapButton(button, &HalGPIO::wasPressed);\n}\n""",
    """bool MappedInputManager::wasPressed(const Button button) const {\n  if (button == Button::Back && syntheticBack) return true;\n#if defined(CROSSPOINT_MURPHY_M4)\n  if (synthKind_ == SynthKind::Key && synthKey_ == button) return true;\n#endif\n  if (keysRoutedToBrowser_ && (button == Button::Back || button == Button::Confirm)) return false;\n  return mapButton(button, &HalGPIO::wasPressed);\n}\n""",
)
replace_once(
    "firmware/src/MappedInputManager.cpp",
    """bool MappedInputManager::wasReleased(const Button button) const {\n  // Pulse both pressed+released so activities using either edge work in one frame.\n  if (button == Button::Back && syntheticBack) return true;\n#if defined(CROSSPOINT_MURPHY_M4)\n  if (synthKind_ == SynthKind::Key && synthKey_ == button) return true;\n\n  // Fullscreen provider/login pages draw an activity-owned four-slot footer.\n""",
    """bool MappedInputManager::wasReleased(const Button button) const {\n  // Pulse both pressed+released so activities using either edge work in one frame.\n  if (button == Button::Back && syntheticBack) return true;\n#if defined(CROSSPOINT_MURPHY_M4)\n  if (synthKind_ == SynthKind::Key && synthKey_ == button) return true;\n  if (keysRoutedToBrowser_ && (button == Button::Back || button == Button::Confirm)) return false;\n\n  // Fullscreen provider/login pages draw an activity-owned four-slot footer.\n""",
)
replace_once(
    "firmware/src/MappedInputManager.cpp",
    """bool MappedInputManager::isPressed(const Button button) const { return mapButton(button, &HalGPIO::isPressed); }\n""",
    """bool MappedInputManager::isPressed(const Button button) const {\n  if (keysRoutedToBrowser_ && (button == Button::Back || button == Button::Confirm)) return false;\n  return mapButton(button, &HalGPIO::isPressed);\n}\n""",
)

# ---------------------------------------------------------------------------
# Firmware receiver: main-loop producer + receiver-task-only socket writer.
# ---------------------------------------------------------------------------
replace_once(
    "firmware/src/network/M4B3TcpReceiver.cpp",
    """#include \"apps/providers/M4Psram.h\"\n#include \"network/M4B3DiscoveryAdvertise.h\"\n#include \"network/M4B3Panel.h\"\n#include \"util/M4B3Input.h\"\n""",
    """#include \"apps/providers/M4Psram.h\"\n#include \"CrossPointSettings.h\"\n#include \"network/M4B3DiscoveryAdvertise.h\"\n#include \"network/M4B3Panel.h\"\n#include \"util/M4B3Input.h\"\n#include \"util/M4B3Key.h\"\n""",
)
replace_once(
    "firmware/src/network/M4B3TcpReceiver.cpp",
    """  uint8_t tx[64] = {};\n  uint8_t touchTx[M4B3::kEnvelopeSize + M4B3::kTouchHeaderSize] = {};\n""",
    """  uint8_t tx[64] = {};\n  uint8_t touchTx[M4B3::kEnvelopeSize + M4B3::kTouchHeaderSize] = {};\n  uint8_t keyTx[M4B3::kEnvelopeSize + M4B3::kInputKeyHeaderSize] = {};\n""",
)
replace_once(
    "firmware/src/network/M4B3TcpReceiver.cpp",
    """  M4B3Input::Queue input;\n  uint32_t touchEnvSeq = 0;\n  uint32_t touchTxErr = 0;\n  uint32_t touchLastLatencyMs = 0;\n""",
    """  M4B3Input::Queue input;\n  M4B3Key::Queue keys;\n  uint32_t touchEnvSeq = 0;\n  uint32_t touchTxErr = 0;\n  uint32_t touchLastLatencyMs = 0;\n  uint32_t keyEnvSeq = 0;\n  uint32_t keyTxErr = 0;\n  uint32_t keyLastLatencyMs = 0;\n""",
)
replace_once(
    "firmware/src/network/M4B3TcpReceiver.cpp",
    """  s.touchCapture = gRt.connected.load(std::memory_order_relaxed) && gRt.session.helloOk();\n}\n""",
    """  s.touchCapture = gRt.connected.load(std::memory_order_relaxed) && gRt.session.helloOk();\n  portENTER_CRITICAL(&gRt.inputMux);\n  const M4B3Key::Stats key = gRt.keys.stats();\n  s.keyBack = key.back;\n  s.keyReload = key.reload;\n  s.keyRejected = key.rejected;\n  s.keyOverflow = key.overflow;\n  s.keySession = gRt.keys.session();\n  s.keySessionResets = key.sessionResets;\n  s.keyLastSeq = gRt.keys.lastSeq();\n  s.keyLastAction = gRt.keys.lastAction();\n  s.keyQueue = static_cast<uint8_t>(gRt.keys.size());\n  portEXIT_CRITICAL(&gRt.inputMux);\n  s.keyTxErr = gRt.keyTxErr;\n  s.keyLastLatencyMs = gRt.keyLastLatencyMs;\n  s.keyCapture = s.touchCapture;\n}\n""",
)
replace_once(
    "firmware/src/network/M4B3TcpReceiver.cpp",
    """      \"touch(d=%u m=%u u=%u c=%u coal=%u dropM=%u rej=%u q=%u act=%d sess=%u)\\n\",\n""",
    """      \"touch(d=%u m=%u u=%u c=%u coal=%u dropM=%u rej=%u q=%u act=%d sess=%u) \"\n      \"key(back=%u reload=%u rej=%u ovf=%u q=%u sess=%u txerr=%u)\\n\",\n""",
)
replace_once(
    "firmware/src/network/M4B3TcpReceiver.cpp",
    """      s.touchDroppedMove, s.touchRejected, static_cast<unsigned>(s.touchQueue), s.touchActive ? 1 : 0,\n      s.touchSession);\n""",
    """      s.touchDroppedMove, s.touchRejected, static_cast<unsigned>(s.touchQueue), s.touchActive ? 1 : 0,\n      s.touchSession, s.keyBack, s.keyReload, s.keyRejected, s.keyOverflow,\n      static_cast<unsigned>(s.keyQueue), s.keySession, s.keyTxErr);\n""",
)
replace_once(
    "firmware/src/network/M4B3TcpReceiver.cpp",
    """void resetInputSession() {\n  portENTER_CRITICAL(&gRt.inputMux);\n  gRt.input.resetSession();\n  gRt.haveLastPanel = false;\n  portEXIT_CRITICAL(&gRt.inputMux);\n}\n""",
    """void resetInputSession() {\n  portENTER_CRITICAL(&gRt.inputMux);\n  gRt.input.resetSession();\n  gRt.keys.resetSession();\n  gRt.haveLastPanel = false;\n  portEXIT_CRITICAL(&gRt.inputMux);\n}\n""",
)
replace_once(
    "firmware/src/network/M4B3TcpReceiver.cpp",
    """    gRt.session.stats().bytesTx += static_cast<uint32_t>(n);\n    gRt.touchLastLatencyMs = now >= ev.tMs ? (now - ev.tMs) : 0;\n  }\n}\n\nbool staReady""",
    """    gRt.session.stats().bytesTx += static_cast<uint32_t>(n);\n    gRt.touchLastLatencyMs = now >= ev.tMs ? (now - ev.tMs) : 0;\n  }\n  for (;;) {\n    M4B3Key::Event ev;\n    portENTER_CRITICAL(&gRt.inputMux);\n    const bool have = gRt.keys.pop(ev);\n    portEXIT_CRITICAL(&gRt.inputMux);\n    if (!have) return;\n    const size_t n = M4B3::encodeInputKey(gRt.keyTx, sizeof(gRt.keyTx), gRt.keyEnvSeq++, ev.action,\n                                          ev.flags, ev.tMs, ev.seq, ev.session);\n    if (n == 0) {\n      gRt.keyTxErr++;\n      continue;\n    }\n    const size_t wrote = gRt.client.write(gRt.keyTx, n);\n    if (wrote != n) {\n      gRt.keyTxErr++;\n      return;\n    }\n    gRt.session.stats().bytesTx += static_cast<uint32_t>(n);\n    gRt.keyLastLatencyMs = now >= ev.tMs ? (now - ev.tMs) : 0;\n  }\n}\n\nbool staReady""",
)
replace_once(
    "firmware/src/network/M4B3TcpReceiver.cpp",
    """  if (gpio.wasTouchReleased()) {\n    (void)gRt.input.push(M4B3::kTouchUp, x, y, nowMs);\n  }\n  portEXIT_CRITICAL(&gRt.inputMux);\n}\n""",
    """  if (gpio.wasTouchReleased()) {\n    (void)gRt.input.push(M4B3::kTouchUp, x, y, nowMs);\n  }\n  portEXIT_CRITICAL(&gRt.inputMux);\n\n  // Logical Browser controls follow the user's front-button remapping. Emit on\n  // release so one physical click produces exactly one Browser action. The\n  // MappedInputManager suppresses these same physical Back/Confirm events from\n  // the hidden local Activity while Browser Bridge owns input.\n  const uint8_t backHw = SETTINGS.frontButtonBack;\n  const uint8_t reloadHw = SETTINGS.frontButtonConfirm;\n  portENTER_CRITICAL(&gRt.inputMux);\n  if (gpio.wasReleased(backHw)) {\n    (void)gRt.keys.push(M4B3::kInputKeyBack, nowMs);\n  }\n  if (reloadHw != backHw && gpio.wasReleased(reloadHw)) {\n    (void)gRt.keys.push(M4B3::kInputKeyReload, nowMs);\n  }\n  portEXIT_CRITICAL(&gRt.inputMux);\n}\n""",
)

replace_once(
    "firmware/src/network/M4B3TcpReceiver.h",
    """  bool touchActive = false;\n  bool touchCapture = false;\n};\n""",
    """  bool touchActive = false;\n  bool touchCapture = false;\n  uint32_t keyBack = 0;\n  uint32_t keyReload = 0;\n  uint32_t keyRejected = 0;\n  uint32_t keyOverflow = 0;\n  uint32_t keyTxErr = 0;\n  uint32_t keySession = 0;\n  uint32_t keySessionResets = 0;\n  uint32_t keyLastSeq = 0;\n  uint32_t keyLastLatencyMs = 0;\n  uint8_t keyLastAction = 0;\n  uint8_t keyQueue = 0;\n  bool keyCapture = false;\n};\n""",
)

# ---------------------------------------------------------------------------
# Main-loop routing must be armed immediately after gpio.update(), before the
# existing global Back/Confirm semantics inspect physical state.
# ---------------------------------------------------------------------------
replace_once(
    "firmware/src/main.cpp",
    """  gpio.update();\n\n#ifndef CROSSPOINT_X3\n""",
    """  gpio.update();\n\n#ifdef CROSSPOINT_MURPHY_M4\n  // Browser input ownership must be decided before any local/global button\n  // semantics run. Raw HAL edges remain available to M4B3Tcp::captureFromGpio;\n  // MappedInputManager suppresses Browser-owned Back/Confirm from local UI.\n  const bool browserInput = M4B3Tcp::inputCaptureActive();\n  mappedInputManager.setTouchRoutedToBrowser(browserInput);\n  mappedInputManager.setKeysRoutedToBrowser(browserInput);\n  if (browserInput) {\n    M4B3Tcp::captureFromGpio(gpio, millis());\n  }\n#endif\n\n#ifndef CROSSPOINT_X3\n""",
)
replace_once(
    "firmware/src/main.cpp",
    """  // Route the raw FT6x36 pointer into M4B3 only while a Browser session is\n  // hello-ok. Reader/Home gestures stay on the existing path otherwise.\n  const bool browserTouch = M4B3Tcp::inputCaptureActive();\n  mappedInputManager.setTouchRoutedToBrowser(browserTouch);\n  if (browserTouch) {\n    M4B3Tcp::captureFromGpio(gpio, millis());\n  }\n\n""",
    """  // Physical Browser input was routed immediately after gpio.update() so\n  // global/local Back semantics cannot race the returned M4B3 key event.\n\n""",
)

# ---------------------------------------------------------------------------
# Android codec/model.
# ---------------------------------------------------------------------------
replace_once(
    "android/m4-screen-bridge/app/src/main/java/com/murphy/m4screenbridge/browser/stream/M4B3.java",
    """    public static final int TYPE_PONG = 6;\n    public static final int TYPE_TOUCH = 7;\n""",
    """    public static final int TYPE_PONG = 6;\n    public static final int TYPE_TOUCH = 7;\n    public static final int TYPE_INPUT_KEY = 8;\n""",
)
replace_once(
    "android/m4-screen-bridge/app/src/main/java/com/murphy/m4screenbridge/browser/stream/M4B3.java",
    """    public static final int PING_HEADER_SIZE = 4;\n    public static final int TOUCH_HEADER_SIZE = 20;\n    public static final int RECT_META_SIZE = 12;\n""",
    """    public static final int PING_HEADER_SIZE = 4;\n    public static final int TOUCH_HEADER_SIZE = 20;\n    public static final int INPUT_KEY_HEADER_SIZE = 16;\n    public static final int RECT_META_SIZE = 12;\n""",
)
replace_once(
    "android/m4-screen-bridge/app/src/main/java/com/murphy/m4screenbridge/browser/stream/M4B3.java",
    """    public static final int TOUCH_CANCEL = 4;\n\n    /** Tight bound""",
    """    public static final int TOUCH_CANCEL = 4;\n\n    public static final int INPUT_KEY_BACK = 1;\n    public static final int INPUT_KEY_RELOAD = 2;\n\n    /** Tight bound""",
)
replace_once(
    "android/m4-screen-bridge/app/src/main/java/com/murphy/m4screenbridge/browser/stream/M4B3.java",
    """        return type >= TYPE_HELLO && type <= TYPE_TOUCH;\n""",
    """        return type >= TYPE_HELLO && type <= TYPE_INPUT_KEY;\n""",
)
replace_once(
    "android/m4-screen-bridge/app/src/main/java/com/murphy/m4screenbridge/browser/stream/M4B3.java",
    """    public static boolean validTouchAction(int action) {\n        return action >= TOUCH_DOWN && action <= TOUCH_CANCEL;\n    }\n\n    public static String typeName""",
    """    public static boolean validTouchAction(int action) {\n        return action >= TOUCH_DOWN && action <= TOUCH_CANCEL;\n    }\n\n    public static boolean validInputKeyAction(int action) {\n        return action == INPUT_KEY_BACK || action == INPUT_KEY_RELOAD;\n    }\n\n    public static String typeName""",
)
replace_once(
    "android/m4-screen-bridge/app/src/main/java/com/murphy/m4screenbridge/browser/stream/M4B3.java",
    """            case TYPE_TOUCH: return \"TOUCH\";\n            default: return \"UNKNOWN(\" + type + \")\";\n""",
    """            case TYPE_TOUCH: return \"TOUCH\";\n            case TYPE_INPUT_KEY: return \"INPUT_KEY\";\n            default: return \"UNKNOWN(\" + type + \")\";\n""",
)

replace_once(
    "android/m4-screen-bridge/app/src/main/java/com/murphy/m4screenbridge/browser/stream/M4B3Message.java",
    """    public final Ack ack;\n    public final Touch touch;\n    public final long nonce;\n\n    private M4B3Message(int type, int flags, long seq, Hello hello, Keyframe keyframe,\n            Patch patch, Ack ack, Touch touch, long nonce) {\n""",
    """    public final Ack ack;\n    public final Touch touch;\n    public final InputKey inputKey;\n    public final long nonce;\n\n    private M4B3Message(int type, int flags, long seq, Hello hello, Keyframe keyframe,\n            Patch patch, Ack ack, Touch touch, InputKey inputKey, long nonce) {\n""",
)
replace_once(
    "android/m4-screen-bridge/app/src/main/java/com/murphy/m4screenbridge/browser/stream/M4B3Message.java",
    """        this.ack = ack;\n        this.touch = touch;\n        this.nonce = nonce;\n""",
    """        this.ack = ack;\n        this.touch = touch;\n        this.inputKey = inputKey;\n        this.nonce = nonce;\n""",
)
# All factories gain a null inputKey before nonce.
for old, new in [
    ("new M4B3Message(M4B3.TYPE_HELLO, flags, seq, hello, null, null, null, null, 0)",
     "new M4B3Message(M4B3.TYPE_HELLO, flags, seq, hello, null, null, null, null, null, 0)"),
    ("new M4B3Message(M4B3.TYPE_FRAME_KEY, flags, seq, null, keyframe, null, null, null, 0)",
     "new M4B3Message(M4B3.TYPE_FRAME_KEY, flags, seq, null, keyframe, null, null, null, null, 0)"),
    ("new M4B3Message(M4B3.TYPE_FRAME_PATCH, flags, seq, null, null, patch, null, null, 0)",
     "new M4B3Message(M4B3.TYPE_FRAME_PATCH, flags, seq, null, null, patch, null, null, null, 0)"),
    ("new M4B3Message(M4B3.TYPE_FRAME_ACK, flags, seq, null, null, null, ack, null, 0)",
     "new M4B3Message(M4B3.TYPE_FRAME_ACK, flags, seq, null, null, null, ack, null, null, 0)"),
    ("new M4B3Message(M4B3.TYPE_PING, flags, seq, null, null, null, null, null, nonce)",
     "new M4B3Message(M4B3.TYPE_PING, flags, seq, null, null, null, null, null, null, nonce)"),
    ("new M4B3Message(M4B3.TYPE_PONG, flags, seq, null, null, null, null, null, nonce)",
     "new M4B3Message(M4B3.TYPE_PONG, flags, seq, null, null, null, null, null, null, nonce)"),
    ("new M4B3Message(M4B3.TYPE_TOUCH, flags, seq, null, null, null, null, touch, 0)",
     "new M4B3Message(M4B3.TYPE_TOUCH, flags, seq, null, null, null, null, touch, null, 0)"),
]:
    replace_once("android/m4-screen-bridge/app/src/main/java/com/murphy/m4screenbridge/browser/stream/M4B3Message.java", old, new)
replace_once(
    "android/m4-screen-bridge/app/src/main/java/com/murphy/m4screenbridge/browser/stream/M4B3Message.java",
    """    public static M4B3Message touch(int flags, long seq, Touch touch) {\n        return new M4B3Message(M4B3.TYPE_TOUCH, flags, seq, null, null, null, null, touch, null, 0);\n    }\n\n    public boolean isFrame()""",
    """    public static M4B3Message touch(int flags, long seq, Touch touch) {\n        return new M4B3Message(M4B3.TYPE_TOUCH, flags, seq, null, null, null, null, touch, null, 0);\n    }\n\n    public static M4B3Message inputKey(int flags, long seq, InputKey inputKey) {\n        return new M4B3Message(M4B3.TYPE_INPUT_KEY, flags, seq, null, null, null, null, null, inputKey, 0);\n    }\n\n    public boolean isFrame()""",
)
replace_once(
    "android/m4-screen-bridge/app/src/main/java/com/murphy/m4screenbridge/browser/stream/M4B3Message.java",
    """    public static final class Touch {\n""",
    """    public static final class InputKey {\n        public final int action;\n        public final int flags;\n        public final long tMs;\n        public final long inputSeq;\n        public final long session;\n\n        public InputKey(int action, int flags, long tMs, long inputSeq, long session) {\n            this.action = action;\n            this.flags = flags;\n            this.tMs = tMs;\n            this.inputSeq = inputSeq;\n            this.session = session;\n        }\n    }\n\n    public static final class Touch {\n""",
)

replace_once(
    "android/m4-screen-bridge/app/src/main/java/com/murphy/m4screenbridge/browser/stream/M4B3Codec.java",
    """    public static byte[] encodeTouch(M4B3Message.Touch touch, long seq) {\n""",
    """    public static byte[] encodeInputKey(M4B3Message.InputKey key, long seq) {\n        if (key == null) throw M4B3Exception.invalid(\"input key is null\");\n        if (!M4B3.validInputKeyAction(key.action)) {\n            throw M4B3Exception.invalid(\"input key action \" + key.action);\n        }\n        ByteBuffer header = le(M4B3.INPUT_KEY_HEADER_SIZE);\n        header.put((byte) key.action);\n        header.put((byte) key.flags);\n        header.putShort((short) 0);\n        putU32(header, key.tMs);\n        putU32(header, key.inputSeq);\n        putU32(header, key.session);\n        return wrap(M4B3.TYPE_INPUT_KEY, 0, header.array(), new byte[0], seq);\n    }\n\n    public static byte[] encodeTouch(M4B3Message.Touch touch, long seq) {\n""",
)
replace_once(
    "android/m4-screen-bridge/app/src/main/java/com/murphy/m4screenbridge/browser/stream/M4B3Codec.java",
    """            case M4B3.TYPE_TOUCH:\n                return M4B3Message.touch(flags, seq, parseTouch(header, payloadLen));\n            default:\n""",
    """            case M4B3.TYPE_TOUCH:\n                return M4B3Message.touch(flags, seq, parseTouch(header, payloadLen));\n            case M4B3.TYPE_INPUT_KEY:\n                return M4B3Message.inputKey(flags, seq, parseInputKey(header, payloadLen));\n            default:\n""",
)
replace_once(
    "android/m4-screen-bridge/app/src/main/java/com/murphy/m4screenbridge/browser/stream/M4B3Codec.java",
    """    private static M4B3Message.Touch parseTouch(byte[] header, int payloadLen) {\n""",
    """    private static M4B3Message.InputKey parseInputKey(byte[] header, int payloadLen) {\n        if (payloadLen != 0) throw M4B3Exception.invalid(\"INPUT_KEY payload must be empty\");\n        if (header.length != M4B3.INPUT_KEY_HEADER_SIZE) {\n            throw M4B3Exception.invalid(\"INPUT_KEY header_len=\" + header.length);\n        }\n        ByteBuffer h = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);\n        int action = h.get() & 0xFF;\n        int flags = h.get() & 0xFF;\n        h.getShort();\n        long tMs = h.getInt() & 0xFFFFFFFFL;\n        long inputSeq = h.getInt() & 0xFFFFFFFFL;\n        long session = h.getInt() & 0xFFFFFFFFL;\n        if (!M4B3.validInputKeyAction(action)) {\n            throw M4B3Exception.invalid(\"INPUT_KEY action \" + action);\n        }\n        return new M4B3Message.InputKey(action, flags, tMs, inputSeq, session);\n    }\n\n    private static M4B3Message.Touch parseTouch(byte[] header, int payloadLen) {\n""",
)

# ---------------------------------------------------------------------------
# Android owned-WebView dispatch. No global KeyEvent / Accessibility injection.
# ---------------------------------------------------------------------------
replace_once(
    "android/m4-screen-bridge/app/src/main/java/com/murphy/m4screenbridge/browser/BrowserPresentation.java",
    """    boolean dispatchBrowserTouch(MotionEvent event) {\n        return webView != null && event != null && webView.dispatchTouchEvent(event);\n    }\n\n    JsProbe jsProbe()""",
    """    boolean dispatchBrowserTouch(MotionEvent event) {\n        return webView != null && event != null && webView.dispatchTouchEvent(event);\n    }\n\n    boolean goBackInBrowser() {\n        if (webView == null || !webView.canGoBack()) return false;\n        webView.goBack();\n        return true;\n    }\n\n    boolean reloadBrowser() {\n        if (webView == null) return false;\n        webView.reload();\n        return true;\n    }\n\n    JsProbe jsProbe()""",
)
replace_once(
    "android/m4-screen-bridge/app/src/main/java/com/murphy/m4screenbridge/browser/VirtualBrowserSession.java",
    """import com.murphy.m4screenbridge.browser.stream.M4B3InputState;\nimport com.murphy.m4screenbridge.browser.stream.M4B3Message;\n""",
    """import com.murphy.m4screenbridge.browser.stream.M4B3InputState;\nimport com.murphy.m4screenbridge.browser.stream.M4B3KeyState;\nimport com.murphy.m4screenbridge.browser.stream.M4B3Message;\n""",
)
replace_once(
    "android/m4-screen-bridge/app/src/main/java/com/murphy/m4screenbridge/browser/VirtualBrowserSession.java",
    """    private final M4B3InputState inputState = new M4B3InputState();\n    private volatile long inputDispatched;\n    private volatile String inputSnap = \"\";\n""",
    """    private final M4B3InputState inputState = new M4B3InputState();\n    private final M4B3KeyState keyState = new M4B3KeyState();\n    private volatile long inputDispatched;\n    private volatile String inputSnap = \"\";\n    private volatile long keyDispatched;\n    private volatile long keyUnhandled;\n""",
)
replace_once(
    "android/m4-screen-bridge/app/src/main/java/com/murphy/m4screenbridge/browser/VirtualBrowserSession.java",
    """        inputState.reset();\n        inputDispatched = 0;\n        inputSnap = \"\";\n""",
    """        inputState.reset();\n        keyState.reset();\n        inputDispatched = 0;\n        inputSnap = \"\";\n        keyDispatched = 0;\n        keyUnhandled = 0;\n""",
)
replace_once(
    "android/m4-screen-bridge/app/src/main/java/com/murphy/m4screenbridge/browser/VirtualBrowserSession.java",
    """        sb.append(\"\\n\").append(inputSnap.isEmpty() ? inputState.snapshot() : inputSnap)\n                .append(\" dispatched=\").append(inputDispatched);\n""",
    """        sb.append(\"\\n\").append(inputSnap.isEmpty() ? inputState.snapshot() : inputSnap)\n                .append(\" dispatched=\").append(inputDispatched);\n        sb.append(\"\\n\").append(keyState.snapshot())\n                .append(\" dispatched=\").append(keyDispatched)\n                .append(\" unhandled=\").append(keyUnhandled);\n""",
)
replace_once(
    "android/m4-screen-bridge/app/src/main/java/com/murphy/m4screenbridge/browser/VirtualBrowserSession.java",
    """                main.post(() -> cancelActivePointer(\"tcp-disconnect\"));\n                refreshProtocolStats();\n""",
    """                main.post(() -> {\n                    cancelActivePointer(\"tcp-disconnect\");\n                    keyState.onTransportLost();\n                });\n                refreshProtocolStats();\n""",
)
replace_once(
    "android/m4-screen-bridge/app/src/main/java/com/murphy/m4screenbridge/browser/VirtualBrowserSession.java",
    """        if (msg.type == M4B3.TYPE_TOUCH) {\n            main.post(() -> dispatchTouch(msg.touch));\n            return;\n        }\n        M4B3Sender s = sender;\n""",
    """        if (msg.type == M4B3.TYPE_TOUCH) {\n            main.post(() -> dispatchTouch(msg.touch));\n            return;\n        }\n        if (msg.type == M4B3.TYPE_INPUT_KEY) {\n            main.post(() -> dispatchInputKey(msg.inputKey));\n            return;\n        }\n        M4B3Sender s = sender;\n""",
)
replace_once(
    "android/m4-screen-bridge/app/src/main/java/com/murphy/m4screenbridge/browser/VirtualBrowserSession.java",
    """    private void dispatchTouch(M4B3Message.Touch touch) {\n""",
    """    private void dispatchInputKey(M4B3Message.InputKey key) {\n        if (!keyState.accept(key)) return;\n        BrowserPresentation p = presentation;\n        boolean handled = false;\n        if (p != null && key.action == M4B3.INPUT_KEY_BACK) {\n            handled = p.goBackInBrowser();\n        } else if (p != null && key.action == M4B3.INPUT_KEY_RELOAD) {\n            handled = p.reloadBrowser();\n        }\n        if (handled) keyDispatched++;\n        else keyUnhandled++;\n        Log.i(TAG, String.format(Locale.ROOT,\n                \"key %s seq=%d sess=%d handled=%d dispatched=%d unhandled=%d\",\n                key.action == M4B3.INPUT_KEY_BACK ? \"BACK\" : \"RELOAD\",\n                key.inputSeq, key.session, handled ? 1 : 0, keyDispatched, keyUnhandled));\n    }\n\n    private void dispatchTouch(M4B3Message.Touch touch) {\n""",
)
replace_once(
    "android/m4-screen-bridge/app/src/main/java/com/murphy/m4screenbridge/browser/VirtualBrowserSession.java",
    """        active = false;\n        cancelActivePointer(\"session-stop\");\n""",
    """        active = false;\n        cancelActivePointer(\"session-stop\");\n        keyState.onTransportLost();\n""",
)

# ---------------------------------------------------------------------------
# Pure-Java test gate.
# ---------------------------------------------------------------------------
replace_once(
    "android/m4-screen-bridge/build.sh",
    """    \"$STREAM_SRC/M4B3InputState.java\" \\\n    \"$SRC/browser/discovery/M4LanDiscovery.java\" \\\n""",
    """    \"$STREAM_SRC/M4B3InputState.java\" \\\n    \"$STREAM_SRC/M4B3KeyState.java\" \\\n    \"$SRC/browser/discovery/M4LanDiscovery.java\" \\\n""",
)
replace_once(
    "android/m4-screen-bridge/build.sh",
    """    \"$TEST_SRC/M4B3InputTest.java\" \\\n    \"$TEST_SRC/M4LanDiscoveryTest.java\"\n""",
    """    \"$TEST_SRC/M4B3InputTest.java\" \\\n    \"$TEST_SRC/M4B3KeyTest.java\" \\\n    \"$TEST_SRC/M4LanDiscoveryTest.java\"\n""",
)
replace_once(
    "android/m4-screen-bridge/build.sh",
    """  java -cp \"$OUT/testclasses\" com.murphy.m4screenbridge.M4B3InputTest\n  java -cp \"$OUT/testclasses\" com.murphy.m4screenbridge.M4LanDiscoveryTest\n""",
    """  java -cp \"$OUT/testclasses\" com.murphy.m4screenbridge.M4B3InputTest\n  java -cp \"$OUT/testclasses\" com.murphy.m4screenbridge.M4B3KeyTest\n  java -cp \"$OUT/testclasses\" com.murphy.m4screenbridge.M4LanDiscoveryTest\n""",
)

print("M5 Browser Bridge key-return integration applied exactly.")
