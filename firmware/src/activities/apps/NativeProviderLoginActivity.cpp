#include "NativeProviderLoginActivity.h"

#include "MappedInputManager.h"
#include "apps/providers/M4NativeProviderLogin.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"
#include "util/QRCodeHelper.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>

#include <algorithm>

NativeProviderLoginActivity::NativeProviderLoginActivity(
    GfxRenderer& renderer, MappedInputManager& mappedInput, std::string providerId,
    std::string appDataRoot, const std::function<void(bool)>& onFinished)
    : Activity("NativeProviderLogin", renderer, mappedInput),
      providerId_(std::move(providerId)),
      appDataRoot_(std::move(appDataRoot)),
      onFinished_(onFinished) {}

void NativeProviderLoginActivity::onEnter() {
  Activity::onEnter();
  delivered_ = false;
  lastSignature_.clear();
  lastPaintMs_ = 0;
  startFailed_ = !M4NativeProviderLogin::start(providerId_, appDataRoot_);
  render(true);
}

void NativeProviderLoginActivity::onExit() {
  M4NativeProviderLogin::cancel();
  Activity::onExit();
}

void NativeProviderLoginActivity::loop() {
  const auto snap = M4NativeProviderLogin::snapshot();
  render(false);

  if (startFailed_) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) {
      if (!delivered_) {
        delivered_ = true;
        onFinished_(false);
      }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) &&
        !M4NativeProviderLogin::busy()) {
      startFailed_ = !M4NativeProviderLogin::start(providerId_, appDataRoot_);
      lastSignature_.clear();
      render(true);
    }
    return;
  }

  if (!delivered_ && snap.phase == M4NativeProviderLogin::Phase::Success) {
    delivered_ = true;
    onFinished_(true);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) {
    M4NativeProviderLogin::cancel();
    if (!delivered_) {
      delivered_ = true;
      onFinished_(false);
    }
    return;
  }

  if ((snap.phase == M4NativeProviderLogin::Phase::Error ||
       snap.phase == M4NativeProviderLogin::Phase::Cancelled) &&
      mappedInput.wasReleased(MappedInputManager::Button::Confirm) && !M4NativeProviderLogin::busy()) {
    (void)M4NativeProviderLogin::start(providerId_, appDataRoot_);
    lastSignature_.clear();
    render(true);
  }
}

void NativeProviderLoginActivity::render(bool force) {
  const auto snap = M4NativeProviderLogin::snapshot();
  const uint32_t now = millis();
  const std::string sig = std::to_string(static_cast<int>(snap.phase)) + "|" + snap.qrUrl + "|" +
                          snap.status + "|" + snap.error;
  if (!force && sig == lastSignature_ && now - lastPaintMs_ < 1000) return;
  lastSignature_ = sig;
  lastPaintMs_ = now;

  renderer.clearScreen();
  const auto metrics = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, w, metrics.headerHeight},
                 providerId_ == "weread" ? "微信读书登录" : "晋江登录");

  if (startFailed_) {
    const char* message = M4NativeProviderLogin::busy() ? "上一项登录仍在结束，请稍后重试"
                                                        : "登录任务启动失败，请重试";
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, 220, "登录暂时不可用", true, EpdFontFamily::BOLD);
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 285, message);
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 340, "确认键重试");
  } else if (snap.phase == M4NativeProviderLogin::Phase::WaitingScan && !snap.qrUrl.empty()) {
    constexpr uint8_t px = 5;  // V10 worst case 57*5 = 285 px, safe on 480-wide M4.
    constexpr int maxQr = 57 * px;
    const int x = std::max(12, (w - maxQr) / 2);
    const int y = 145;
    if (!QRCodeHelper::drawQRCode(renderer, x, y, snap.qrUrl, px)) {
      M4UiText::drawCentered(renderer, UI_12_FONT_ID, 260, "二维码生成失败", true, EpdFontFamily::BOLD);
    }
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 455, snap.status.c_str());
    const uint32_t elapsed = snap.startedMs ? (now - snap.startedMs) / 1000u : 0;
    const std::string wait = std::string("等待确认 · ") + std::to_string(elapsed) + "s";
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 495, wait.c_str());
  } else if (snap.phase == M4NativeProviderLogin::Phase::Success) {
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, 245, "登录成功", true, EpdFontFamily::BOLD);
  } else if (snap.phase == M4NativeProviderLogin::Phase::Error) {
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, 220, "登录失败", true, EpdFontFamily::BOLD);
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 275, snap.error.c_str());
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 330, "确认键重试");
  } else if (snap.phase == M4NativeProviderLogin::Phase::Cancelled) {
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, 245, "登录已取消", true, EpdFontFamily::BOLD);
  } else {
    const char* status = snap.status.empty() ? "准备登录…" : snap.status.c_str();
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, 245, status, true, EpdFontFamily::BOLD);
  }

  const bool retry = startFailed_ || snap.phase == M4NativeProviderLogin::Phase::Error ||
                     snap.phase == M4NativeProviderLogin::Phase::Cancelled;
  const auto labels = mappedInput.mapLabels("« 返回", retry ? "重试" : "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

std::string NativeProviderLoginActivity::debugUiJson() {
  const auto s = M4NativeProviderLogin::snapshot();
  return std::string("{\"kind\":\"native_provider_login\",\"provider\":\"") + providerId_ +
         "\",\"phase\":" + std::to_string(static_cast<int>(s.phase)) +
         ",\"has_qr\":" + (s.qrUrl.empty() ? "false" : "true") +
         ",\"start_failed\":" + (startFailed_ ? "true" : "false") + "}";
}
