#!/usr/bin/env python3
"""Apply the ChatGPT-authored M5 key-return control-flow correction exactly once.

This script intentionally performs exact source replacements. If the expected
source does not match, it aborts rather than improvising a different fix.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PATH = ROOT / "firmware/src/network/M4B3TcpReceiver.cpp"


def replace_exact(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one match, found {count}")
    return text.replace(old, new, 1)


text = PATH.read_text(encoding="utf-8")

old_flush = r'''void flushInput() {
  if (!gRt.client || !gRt.connected.load(std::memory_order_relaxed)) return;
  const uint32_t now = millis();
  for (;;) {
    M4B3Input::Event ev;
    portENTER_CRITICAL(&gRt.inputMux);
    const bool have = gRt.input.pop(ev);
    portEXIT_CRITICAL(&gRt.inputMux);
    if (!have) return;
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
'''

new_flush = r'''void flushInput() {
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
'''

old_capture = r'''void captureFromGpio(HalGPIO& gpio, uint32_t nowMs) {
  if (!inputCaptureActive()) return;
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

  // Logical Browser controls follow the user's front-button remapping. Emit on
  // release so one physical click produces exactly one Browser action. The
  // MappedInputManager suppresses these same physical Back/Confirm events from
  // the hidden local Activity while Browser Bridge owns input.
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
}
'''

new_capture = r'''void captureFromGpio(HalGPIO& gpio, uint32_t nowMs) {
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
'''

text = replace_exact(text, old_flush, new_flush, "flushInput")
text = replace_exact(text, old_capture, new_capture, "captureFromGpio")
PATH.write_text(text, encoding="utf-8")
print("applied M5 key control-flow correction: 2 exact replacements")
