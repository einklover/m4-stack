#include "NativeProviderEndpointActivity.h"

#include "MappedInputManager.h"
#include "apps/providers/M4NativeProviderDiscovery.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4FooterTouchPolicy.h"
#include "util/M4UiText.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>

#include <cstdio>
#include <utility>

namespace {

constexpr int kHostTop = 92;
constexpr int kHostHeight = 58;
constexpr int kPortTop = 176;
constexpr int kPortHeight = 58;

std::string jsonEscape(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(static_cast<char>(c));
    } else if (c < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
      out += buf;
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  return out;
}

int phaseNumber(M4LegadoBridge::ManualEndpointPhase phase) {
  return static_cast<int>(phase);
}

}  // namespace

NativeProviderEndpointActivity::NativeProviderEndpointActivity(
    GfxRenderer& renderer, MappedInputManager& mappedInput, std::string providerId,
    std::string appId, const std::function<void(bool)>& onFinished)
    : ActivityWithSubactivity("NativeProviderEndpoint", renderer, mappedInput),
      providerId_(std::move(providerId)),
      appId_(std::move(appId)),
      appDataRoot_(std::string("/apps_data/") + appId_),
      onFinished_(onFinished) {}

uint8_t NativeProviderEndpointActivity::touchFooterButtonsMask() const {
  return M4FooterTouchPolicy::Back | M4FooterTouchPolicy::Confirm |
         M4FooterTouchPolicy::Left | M4FooterTouchPolicy::Right;
}

void NativeProviderEndpointActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  delivered_ = false;
  selectedField_ = Field::Host;
  loadSavedEndpoint();
  render(true);
}

void NativeProviderEndpointActivity::onExit() {
  ActivityWithSubactivity::onExit();
  if (state_.phase == M4LegadoBridge::ManualEndpointPhase::Connecting) {
    M4LegadoBridge::clearBaseUrl();
  }
}

void NativeProviderEndpointActivity::loadSavedEndpoint() {
  host_.clear();
  port_ = "1122";
  state_ = {};

  M4LegadoBridge::ParsedEndpoint parsed;
  const std::string saved = M4LegadoBridge::loadSavedBase(appDataRoot_);
  if (M4LegadoBridge::parseEndpoint(saved, {}, parsed)) {
    host_ = parsed.host;
    port_ = std::to_string(parsed.port);
    state_.lastSuccessful = parsed.base;
    return;
  }

  if (M4LegadoBridge::parseEndpoint(M4LegadoBridge::kDefaultBase, {}, parsed)) {
    host_ = parsed.host;
    port_ = std::to_string(parsed.port);
  } else {
    host_ = "192.168.0.118";
  }
}

void NativeProviderEndpointActivity::editField(Field field) {
  if (state_.phase == M4LegadoBridge::ManualEndpointPhase::Error) {
    state_.phase = M4LegadoBridge::ManualEndpointPhase::Editing;
    state_.error.clear();
  }
  selectedField_ = field;
  const std::string title = field == Field::Host ? "输入服务器地址" : "输入端口";
  const std::string initial = field == Field::Host ? host_ : port_;
  const size_t maxLength = field == Field::Host ? 128u : 5u;
  enterNewActivity(new KeyboardEntryActivity(
      renderer, mappedInput, title, initial, 12, maxLength, false,
      [this, field](const std::string& value) {
        if (field == Field::Host) {
          host_ = value;
        } else {
          port_ = value;
        }
        requestExitSubActivity();
      },
      [this]() { requestExitSubActivity(); }));
}

void NativeProviderEndpointActivity::beginConnect() {
  if (state_.phase == M4LegadoBridge::ManualEndpointPhase::Connecting) return;

  M4LegadoBridge::ParsedEndpoint parsed;
  std::string parseError;
  if (!M4LegadoBridge::parseEndpoint(host_, port_, parsed, &parseError)) {
    state_.fail(connectionError(parseError));
    render(true);
    return;
  }

  host_ = parsed.host;
  port_ = std::to_string(parsed.port);
  state_.begin(parsed.base);
  M4LegadoBridge::setBaseUrl(appDataRoot_, parsed.base, false);
  if (!M4NativeProviderDiscovery::startDefault(providerId_, appId_)) {
    M4LegadoBridge::clearBaseUrl();
    state_.fail(M4NativeProviderDiscovery::busy() ? "已有连接任务，请稍候" : "无法启动连接，请重试");
  }
  render(true);
}

void NativeProviderEndpointActivity::finish(bool ok) {
  if (delivered_) return;
  if (!ok && state_.phase != M4LegadoBridge::ManualEndpointPhase::Ready) {
    M4LegadoBridge::clearBaseUrl();
  }
  delivered_ = true;
  if (onFinished_) onFinished_(ok);
}

std::string NativeProviderEndpointActivity::connectionError(const std::string& code) const {
  if (code == "host_required") return "请输入服务器地址";
  if (code == "invalid_host") return "地址格式无效";
  if (code == "unsupported_scheme") return "仅支持 http:// 地址";
  if (code == "unsupported_path") return "地址不要包含接口路径";
  if (code == "ipv6_not_supported") return "请输入 IPv4 或主机名";
  if (code == "invalid_port") return "端口必须是 1-65535 的数字";
  if (code == "http_404") return "服务接口不存在";
  if (code == "http_401" || code == "http_403") return "服务拒绝访问";
  if (code == "http_begin_failed" || code == "http_request_failed" || code == "wifi_not_connected" ||
      code.find("CONNECT") != std::string::npos) {
    return "无法连接，请检查 Wi-Fi 和地址";
  }
  if (code == "http_timeout" || code == "timeout") return "连接超时，请检查端口";
  if (code.empty() || code == "legado_endpoint_missing") return "未找到开源阅读服务";
  if (code.size() <= 40) return "连接失败 · " + code;
  return "连接失败，请重试";
}

std::string NativeProviderEndpointActivity::phaseKey() const {
  switch (state_.phase) {
    case M4LegadoBridge::ManualEndpointPhase::Connecting: return "connecting";
    case M4LegadoBridge::ManualEndpointPhase::Ready: return "ready";
    case M4LegadoBridge::ManualEndpointPhase::Error: return "error";
    case M4LegadoBridge::ManualEndpointPhase::Editing:
    default: return "editing";
  }
}

void NativeProviderEndpointActivity::monitorConnection() {
  if (state_.phase != M4LegadoBridge::ManualEndpointPhase::Connecting) return;
  const auto discovery = M4NativeProviderDiscovery::snapshot();
  if (discovery.providerId != providerId_ || discovery.appId != appId_) return;
  if (discovery.phase == M4NativeProviderDiscovery::Phase::Ready) {
    M4LegadoBridge::setBaseUrl(appDataRoot_, state_.candidate, true);
    state_.succeed();
    render(true);
    finish(true);
    return;
  }
  if (discovery.phase == M4NativeProviderDiscovery::Phase::Error ||
      discovery.phase == M4NativeProviderDiscovery::Phase::AuthRequired) {
    M4LegadoBridge::clearBaseUrl();
    state_.fail(connectionError(discovery.error));
    render(true);
  }
}

void NativeProviderEndpointActivity::loop() {
  if (subActivity) {
    if (pumpSubActivityFrame()) render(true);
    return;
  }

  monitorConnection();

  if (mappedInput.wasBackGesture() || mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish(false);
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    selectedField_ = selectedField_ == Field::Host ? Field::Port : Field::Host;
    render(true);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    editField(selectedField_);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    beginConnect();
    return;
  }

  int tx = 0;
  int ty = 0;
  if (!mappedInput.wasScreenTapped(tx, ty)) return;
  const auto metrics = UITheme::getInstance().getMetrics();
  const int footerTop = renderer.getScreenHeight() - metrics.buttonHintsHeight;
  if (ty >= footerTop) {
    const int slot = M4FooterTouchPolicy::slotFromPoint(tx, ty, renderer.getScreenWidth(),
                                                        renderer.getScreenHeight(),
                                                        metrics.buttonHintsHeight);
    if (slot == 0) finish(false);
    else if (slot == 1) editField(selectedField_);
    else if (slot == 2 || slot == 3) beginConnect();
    return;
  }
  if (ty >= kHostTop && ty < kHostTop + kHostHeight) {
    editField(Field::Host);
    return;
  }
  if (ty >= kPortTop && ty < kPortTop + kPortHeight) {
    editField(Field::Port);
  }
}

void NativeProviderEndpointActivity::render(bool force) {
  std::string signature = phaseKey() + "|" + host_ + "|" + port_ + "|" + state_.error + "|" + state_.candidate;
  const uint32_t now = millis();
  if (!force && signature == lastSignature_ && now - lastPaintMs_ < 250u) return;
  lastSignature_ = signature;
  lastPaintMs_ = now;

  renderer.clearScreen();
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, 16, "连接开源阅读服务", true, EpdFontFamily::BOLD);
  M4UiText::draw(renderer, UI_10_FONT_ID, 18, 54, "服务器地址（IP、主机名或完整 URL）");
  renderer.drawRect(18, kHostTop, renderer.getScreenWidth() - 36, kHostHeight);
  if (selectedField_ == Field::Host) renderer.fillRectDither(20, kHostTop + 2, renderer.getScreenWidth() - 40, kHostHeight - 4, LightGray);
  const std::string hostDisplay = M4UiText::truncated(renderer, UI_10_FONT_ID, host_.c_str(), renderer.getScreenWidth() - 52);
  M4UiText::draw(renderer, UI_10_FONT_ID, 26, kHostTop + 18, hostDisplay.c_str());

  M4UiText::draw(renderer, UI_10_FONT_ID, 18, 148, "端口");
  renderer.drawRect(18, kPortTop, 190, kPortHeight);
  if (selectedField_ == Field::Port) renderer.fillRectDither(20, kPortTop + 2, 186, kPortHeight - 4, LightGray);
  M4UiText::draw(renderer, UI_10_FONT_ID, 28, kPortTop + 18, port_.c_str());

  std::string status;
  if (state_.phase == M4LegadoBridge::ManualEndpointPhase::Connecting) {
    status = "正在连接 " + state_.candidate;
  } else if (state_.phase == M4LegadoBridge::ManualEndpointPhase::Ready) {
    status = "连接成功，正在返回书架";
  } else if (state_.phase == M4LegadoBridge::ManualEndpointPhase::Error) {
    status = state_.error;
  } else {
    status = state_.lastSuccessful.empty() ? "自动发现失败，请手动输入后连接" : "可修改地址，或直接重试上次成功地址";
  }
  const std::string statusDisplay = M4UiText::truncated(renderer, UI_10_FONT_ID, status.c_str(), renderer.getScreenWidth() - 36);
  M4UiText::draw(renderer, UI_10_FONT_ID, 18, 274, statusDisplay.c_str(),
                 state_.phase != M4LegadoBridge::ManualEndpointPhase::Error);
  M4UiText::draw(renderer, SMALL_FONT_ID, 18, 318, "示例：192.168.1.20 + 1122");
  M4UiText::draw(renderer, SMALL_FONT_ID, 18, 344, "或输入 http://主机名:端口");
  if (!state_.lastSuccessful.empty()) {
    const std::string saved = "上次成功：" + state_.lastSuccessful;
    M4UiText::draw(renderer, SMALL_FONT_ID, 18, 382, saved.c_str());
  }

  const auto labels = mappedInput.mapLabels("返回", "编辑", "连接", "重试");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

std::string NativeProviderEndpointActivity::debugUiJson() {
  return "{\"kind\":\"legado_endpoint\",\"phase\":\"" + phaseKey() +
         "\",\"phase_code\":" + std::to_string(phaseNumber(state_.phase)) +
         ",\"host\":\"" + jsonEscape(host_) + "\",\"port\":\"" + jsonEscape(port_) +
         "\",\"endpoint\":\"" + jsonEscape(state_.candidate) + "\",\"last_successful\":\"" +
         jsonEscape(state_.lastSuccessful) + "\",\"error\":\"" + jsonEscape(state_.error) + "\"}";
}
