#include "NativeAppActivity.h"
#include "NativeProviderBookActivity.h"
#include "NativeProviderLoginActivity.h"
#include "ScreenBridgeActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <SDCardManager.h>

#include "MappedInputManager.h"
#include "apps/native/M4NativeAppControllerFactory.h"
#include "apps/native/M4NativeProviderHomeTemplate.h"
#include "apps/providers/M4NativeProviderDiscovery.h"
#include "apps/providers/M4NativeProviderIo.h"
#include "apps/providers/M4NativeProviderManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4ContentProviderContract.h"
#include "util/M4ErrorScreen.h"
#include "util/M4ListTouchPolicy.h"
#include "util/M4UiText.h"
#include "util/TouchHitGeometry.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>

namespace {

void* uiAlloc(size_t n) {
#if defined(ARDUINO_ARCH_ESP32)
  if (n) {
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;
  }
#endif
  return std::malloc(n);
}

void uiFree(void* p) {
  if (!p) return;
  std::free(p);
}

std::string jsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(static_cast<char>(c));
    } else if (c < 0x20) {
      char b[8];
      std::snprintf(b, sizeof(b), "\\u%04x", static_cast<unsigned>(c));
      out += b;
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  return out;
}

int textDefaultHeight(const M4NativeUi::Node& node) {
  if (M4NativeUi::hasStyle(node.style, M4NativeUi::StyleHero)) return 44;
  if (M4NativeUi::hasStyle(node.style, M4NativeUi::StyleSection)) return 30;
  if (M4NativeUi::hasStyle(node.style, M4NativeUi::StyleMuted)) return 28;
  if (M4NativeUi::hasStyle(node.style, M4NativeUi::StyleCompact)) return 28;
  return 34;
}

int flowDefaultHeight(const M4NativeUi::Node& node) {
  return M4NativeUi::hasStyle(node.style, M4NativeUi::StyleCompact) ? 72 : 96;
}

int tilesDefaultHeight(const M4NativeUi::Node&) { return 116; }

int nodeSidePadding(const M4NativeUi::Node& node, int base) {
  return base + (M4NativeUi::hasStyle(node.style, M4NativeUi::StyleInset) ? 10 : 0);
}

std::string rankedTitle(int index0, const std::string& title) {
  char prefix[8];
  std::snprintf(prefix, sizeof(prefix), "%02d  ", index0 + 1);
  return std::string(prefix) + title;
}

}  // namespace

NativeAppActivity::NativeAppActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     M4xInstalledApp app, const std::function<void()>& onExitApp)
    : ActivityWithSubactivity("NativeApp", renderer, mappedInput),
      app_(std::move(app)),
      onExitApp_(onExitApp),
      controller_(M4NativeAppControllers::create(app_)) {}

void NativeAppActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  error_.clear();
  authLoginPrompted_ = false;
  if (app_.provider == "screenbridge") {
    // ScreenBridge is a dedicated fullscreen reader activity, not an XML/UI
    // document. Closing the child closes the app: the callback only arms a
    // deferred-exit flag that loop() applies after the child frame returns.
    screenBridgeExitPending_ = false;
    enterNewActivity(new ScreenBridgeActivity(renderer, mappedInput, app_.id, [this]() {
      screenBridgeExitPending_ = true;
    }));
    return;
  }
  if (!loadDocument()) {
    updateRequired_ = true;
    return;
  }
  M4TouchNavigation::activateForActivity(showTouchNavigation());
  M4UiRuntimePolicy::setTextScalePercent(uiTextScalePercent());
  screenId_ = document_.startScreen;
  selectedIndex_ = 0;
  tabIndex_ = 0;
  controllerRevision_ = controller_ ? controller_->revision() : 0;
  updateRequired_ = true;

  // WeRead is account-backed, unlike public discovery providers. Do not allow a
  // stale local shelf cache to bypass authentication. Missing local credentials
  // go straight to QR login; existing credentials are actively validated by a
  // shelf sync on every app entry. Discovery AuthRequired is handled in loop().
  if (app_.provider == "weread") {
    const std::string root = std::string("/apps_data/") + app_.id;
    if (!M4NativeProviderIo::hasCredential(root, "weread")) {
      authLoginPrompted_ = true;
      handleAction("provider.login");
      return;
    }
    if (!M4NativeProviderDiscovery::busy()) {
      (void)M4NativeProviderDiscovery::startDefault(app_.provider, app_.id);
    }
  }
}

void NativeAppActivity::onExit() {
  ActivityWithSubactivity::onExit();
  controller_.reset();
  document_ = {};
}

bool NativeAppActivity::loadDocument() {
  M4NativeUi::Limits limits;
  if (const char* builtin = M4NativeProviderHomeTemplate::xmlFor(app_.provider)) {
    const size_t n = std::strlen(builtin);
    auto parsed = M4NativeUi::parse(builtin, n, limits);
    if (!parsed) {
      setError(std::string("ui_parse:") + M4NativeUi::errorKey(parsed.error));
      return false;
    }
    document_ = std::move(parsed.document);
    return true;
  }

  std::string path = app_.path;
  if (!path.empty() && path.back() != '/') path += '/';
  path += app_.entry.empty() ? "main.xml" : app_.entry;

  FsFile f;
  if (!SdMan.openFileForRead("NativeUI", path.c_str(), f)) {
    setError("ui_entry_missing");
    return false;
  }
  const size_t n = f.fileSize();
  if (n == 0 || n > limits.maxBytes) {
    f.close();
    setError(n == 0 ? "ui_entry_empty" : "ui_entry_too_large");
    return false;
  }

  char* buf = static_cast<char*>(uiAlloc(n + 1));
  if (!buf) {
    f.close();
    setError("ui_oom");
    return false;
  }
  size_t off = 0;
  while (off < n) {
    const int got = f.read(reinterpret_cast<uint8_t*>(buf + off), n - off);
    if (got <= 0) break;
    off += static_cast<size_t>(got);
  }
  f.close();
  if (off != n) {
    uiFree(buf);
    setError("ui_read_failed");
    return false;
  }
  buf[n] = '\0';
  auto parsed = M4NativeUi::parse(buf, n, limits);
  uiFree(buf);
  if (!parsed) {
    setError(std::string("ui_parse:") + M4NativeUi::errorKey(parsed.error));
    return false;
  }
  document_ = std::move(parsed.document);
  return true;
}

const M4NativeUi::Screen* NativeAppActivity::currentScreen() const {
  return M4NativeUi::findScreen(document_, screenId_);
}

std::string NativeAppActivity::resolved(const std::string& s) const {
  return controller_ ? M4NativeUi::resolveText(*controller_, s) : s;
}

void NativeAppActivity::setError(const std::string& error) {
  error_ = error;
  updateRequired_ = true;
}

bool NativeAppActivity::rowAt(int index0, M4NativeUi::Row& out) const {
  if (!controller_ || index0 < 0 || index0 >= listCount_ || listSource_.empty()) return false;
  return controller_->rowAt(listSource_, static_cast<size_t>(index0), out);
}

void NativeAppActivity::handleAction(const std::string& action, const M4NativeUi::Node* node, int index0) {
  if (action.empty() || !controller_) return;

  M4NativeUi::ActionContext ctx;
  ctx.screenId = screenId_;
  ctx.index0 = index0;
  if (node) {
    ctx.nodeId = node->id;
    ctx.source = node->source;
  } else {
    ctx.nodeId = listNodeId_;
    ctx.source = listSource_;
  }

  std::string selectedTitle;
  std::string selectedSubtitle;
  if (index0 >= 0 && !ctx.source.empty()) {
    M4NativeUi::Row row;
    if (controller_->rowAt(ctx.source, static_cast<size_t>(index0), row)) {
      ctx.rowKey = row.key;
      selectedTitle = row.title;
      selectedSubtitle = row.subtitle;
    }
  }

  const auto result = controller_->dispatch(action, ctx);
  switch (result.kind) {
    case M4NativeUi::ActionKind::Repaint:
      updateRequired_ = true;
      return;
    case M4NativeUi::ActionKind::Navigate:
      if (!M4NativeUi::findScreen(document_, result.screenId)) {
        setError("ui_bad_route");
        return;
      }
      screenId_ = result.screenId;
      selectedIndex_ = 0;
      tabIndex_ = 0;
      updateRequired_ = true;
      return;
    case M4NativeUi::ActionKind::Close:
      onExitApp_();
      return;
    case M4NativeUi::ActionKind::OpenProviderBook:
    case M4NativeUi::ActionKind::OpenProviderToc: {
      std::string providerId;
      std::string bookId;
      if (!M4ContentProvider::parseHistoryUri(result.payload.c_str(), providerId, bookId)) {
        setError("provider_bad_route");
        return;
      }
      enterNewActivity(new NativeProviderBookActivity(
          renderer, mappedInput, providerId, bookId, app_.id, selectedTitle, selectedSubtitle,
          [this]() {
            requestExitSubActivity();
            updateRequired_ = true;
          }));
      return;
    }
    case M4NativeUi::ActionKind::OpenLogin: {
      const std::string providerId = result.payload.empty() ? app_.provider : result.payload;
      if (providerId.empty()) {
        setError("provider_login_missing_id");
        return;
      }
      const std::string appDataRoot = std::string("/apps_data/") + app_.id;
      enterNewActivity(new NativeProviderLoginActivity(
          renderer, mappedInput, providerId, appDataRoot,
          [this, providerId](bool ok) {
            if (ok) {
              M4NativeProviderManager::acknowledgeAuth(providerId);
              // Successful WeRead login immediately refreshes and validates the
              // shelf instead of waiting for a manual Refresh press.
              if (providerId == "weread" && !M4NativeProviderDiscovery::busy()) {
                (void)M4NativeProviderDiscovery::startDefault(providerId, app_.id);
              }
            }
            requestExitSubActivity();
            updateRequired_ = true;
          }));
      return;
    }
    case M4NativeUi::ActionKind::Error:
      setError(result.error.empty() ? "native_action_failed" : result.error);
      return;
    case M4NativeUi::ActionKind::None:
    default:
      return;
  }
}

void NativeAppActivity::loop() {
  // ScreenBridge child requested app close. Tear the child down here (parent
  // frame, outside the child's own loop stack) before closing the app.
  if (screenBridgeExitPending_) {
    screenBridgeExitPending_ = false;
    exitActivity();
    onExitApp_();
    return;
  }

  if (subActivity) {
    if (pumpSubActivityFrame()) updateRequired_ = true;
    return;
  }

  const auto providerProgress = M4NativeProviderManager::progress();
  const auto discovery = M4NativeProviderDiscovery::snapshot();
  const bool chapterAuthRequired = !app_.provider.empty() && providerProgress.providerId == app_.provider &&
                                   providerProgress.phase == M4NativeProvider::Phase::AuthRequired;
  const bool discoveryAuthRequired = !app_.provider.empty() && discovery.providerId == app_.provider &&
                                     discovery.appId == app_.id &&
                                     discovery.phase == M4NativeProviderDiscovery::Phase::AuthRequired;
  const bool authRequired = chapterAuthRequired || discoveryAuthRequired;
  if (authRequired && !authLoginPrompted_) {
    authLoginPrompted_ = true;
    handleAction("provider.login");
    return;
  }
  if (!authRequired) authLoginPrompted_ = false;

  if (controller_) {
    controller_->pollAsync();
    const uint32_t revision = controller_->revision();
    if (revision != controllerRevision_) {
      controllerRevision_ = revision;
      updateRequired_ = true;
    }
  }

  if (updateRequired_) {
    updateRequired_ = false;
    render();
  }

  if (!error_.empty()) {
    int tx = 0, ty = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) {
      error_.clear();
      updateRequired_ = true;
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(tx, ty)) {
      error_.clear();
      updateRequired_ = true;
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) {
    if (!buttonActions_[0].empty()) handleAction(buttonActions_[0]);
    else onExitApp_();
    return;
  }

  if (listCount_ > 0) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      selectedIndex_ = (selectedIndex_ + listCount_ - 1) % listCount_;
      updateRequired_ = true;
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      selectedIndex_ = (selectedIndex_ + 1) % listCount_;
      updateRequired_ = true;
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      handleAction(listAction_.empty() ? buttonActions_[1] : listAction_, nullptr, selectedIndex_);
      return;
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && !buttonActions_[1].empty()) {
    handleAction(buttonActions_[1]);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left) && !buttonActions_[2].empty()) {
    handleAction(buttonActions_[2]);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right) && !buttonActions_[3].empty()) {
    handleAction(buttonActions_[3]);
    return;
  }

  if (!mappedInput.hasTouch()) return;
  const auto sw = mappedInput.wasSwipe();
  if (listCount_ > 0 && (sw == MappedInputManager::SwipeDir::Up ||
                         sw == MappedInputManager::SwipeDir::Down)) {
    const auto metrics = UITheme::getInstance().getMetrics();
    bool hasSubtitle = true;
    const auto* screen = currentScreen();
    if (screen) {
      for (const auto& n : screen->nodes) {
        if (n.type == M4NativeUi::NodeType::List) {
          hasSubtitle = !n.subtitleField.empty();
          break;
        }
      }
    }
    const int baseRowHeight = hasSubtitle ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight;
    const int rowHeight = M4UiText::listRowHeight(renderer, UI_10_FONT_ID, baseRowHeight, hasSubtitle);
    const int pageItems = std::max(1, listHeight_ / std::max(1, rowHeight));
    selectedIndex_ = M4ListTouchPolicy::applyPage(
        selectedIndex_, listCount_, pageItems, sw == MappedInputManager::SwipeDir::Up);
    updateRequired_ = true;
    return;
  }

  int tx = 0, ty = 0;
  if (!mappedInput.wasScreenTapped(tx, ty)) return;
  const auto metrics = UITheme::getInstance().getMetrics();
  const int footerTop = renderer.getScreenHeight() - metrics.buttonHintsHeight;
  if (ty >= footerTop) {
    const int slot = std::min(3, std::max(0, tx * 4 / std::max(1, renderer.getScreenWidth())));
    if (!buttonActions_[slot].empty()) handleAction(buttonActions_[slot]);
    return;
  }

  if (tilesNode_ && tilesCount_ > 0 && ty >= tilesTop_ && ty < tilesTop_ + tilesHeight_) {
    const int pad = nodeSidePadding(*tilesNode_, metrics.contentSidePadding);
    constexpr int gap = 6;
    const int columns = std::max(1, tilesColumns_);
    const int rows = std::max(1, (tilesCount_ + columns - 1) / columns);
    const int innerWidth = std::max(1, renderer.getScreenWidth() - 2 * pad);
    const int cellWidth = std::max(1, (innerWidth - gap * (columns - 1)) / columns);
    const int cellHeight = std::max(1, (tilesHeight_ - gap * (rows - 1)) / rows);
    const int localX = tx - pad;
    const int localY = ty - tilesTop_;
    if (localX >= 0 && localY >= 0) {
      const int col = localX / std::max(1, cellWidth + gap);
      const int row = localY / std::max(1, cellHeight + gap);
      const int inCellX = localX % std::max(1, cellWidth + gap);
      const int inCellY = localY % std::max(1, cellHeight + gap);
      const int index = row * columns + col;
      if (col >= 0 && col < columns && row >= 0 && row < rows && index >= 0 &&
          index < tilesCount_ && inCellX < cellWidth && inCellY < cellHeight) {
        handleAction(tilesNode_->action, tilesNode_, index);
        return;
      }
    }
  }

  if (listCount_ <= 0) return;
  int hit = -1;
  const auto* screen = currentScreen();
  bool hasSubtitle = true;
  if (screen) {
    for (const auto& n : screen->nodes) {
      if (n.type == M4NativeUi::NodeType::List) {
        hasSubtitle = !n.subtitleField.empty();
        break;
      }
    }
  }
  const int baseRowHeight = hasSubtitle ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight;
  const int rowHeight = M4UiText::listRowHeight(renderer, UI_10_FONT_ID, baseRowHeight, hasSubtitle);
  if (TouchHitGeometry::listIndexFromPoint(ty, listTop_, listHeight_, rowHeight,
                                           listCount_, selectedIndex_, hit)) {
    selectedIndex_ = hit;
    updateRequired_ = true;
    if (!listAction_.empty()) handleAction(listAction_, nullptr, hit);
  }
}

void NativeAppActivity::render() {
  renderer.clearScreen();
  const auto metrics = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight();

  for (auto& a : buttonActions_) a.clear();
  listTop_ = 0;
  listHeight_ = 0;
  listCount_ = 0;
  listSource_.clear();
  listNodeId_.clear();
  listAction_.clear();
  tilesTop_ = 0;
  tilesHeight_ = 0;
  tilesCount_ = 0;
  tilesColumns_ = 4;
  tilesNode_ = nullptr;

  if (!error_.empty()) {
    const auto labels = mappedInput.mapLabels("« 返回", "关闭提示", "", "");
    std::vector<std::string> diag;
    M4ErrorScreen::appendCode(diag, error_);
    M4ErrorScreen::addKV(diag, "app_id: ", app_.id);
    M4ErrorScreen::addKV(diag, "provider: ", app_.provider);
    M4ErrorScreen::addKV(diag, "screen: ", screenId_);
    M4ErrorScreen::addKV(diag, "path: ", app_.path);
    auto snap = M4ErrorScreen::genericFail(app_.name.empty() ? "应用" : app_.name, "操作失败", error_, diag,
                                           labels.btn1, labels.btn2);
    M4ErrorScreen::paint(renderer, snap, true);
    return;
  }

  const auto* screen = currentScreen();
  if (!screen || !controller_) {
    setError("ui_screen_missing");
    return;
  }

  const std::string title = resolved(screen->title.empty() ? "@app.name" : screen->title);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, w, metrics.headerHeight}, title.c_str());

  const bool documentCompact = document_.theme == "wap" ||
                               M4NativeUi::hasStyle(screen->style, M4NativeUi::StyleCompact);
  const int screenGap = documentCompact ? std::max(2, metrics.verticalSpacing / 2) : metrics.verticalSpacing;
  int y = metrics.topPadding + metrics.headerHeight + screenGap;
  const int contentBottom = h - metrics.buttonHintsHeight - screenGap;
  int fixedHeight = 0;
  const M4NativeUi::Node* flexList = nullptr;
  for (const auto& node : screen->nodes) {
    switch (node.type) {
      case M4NativeUi::NodeType::Text: fixedHeight += node.height > 0 ? node.height : textDefaultHeight(node); break;
      case M4NativeUi::NodeType::FlowText: fixedHeight += node.height > 0 ? node.height : flowDefaultHeight(node); break;
      case M4NativeUi::NodeType::Tiles: fixedHeight += node.height > 0 ? node.height : tilesDefaultHeight(node); break;
      case M4NativeUi::NodeType::Tabs: fixedHeight += node.height > 0 ? node.height : metrics.tabBarHeight; break;
      case M4NativeUi::NodeType::Progress: fixedHeight += node.height > 0 ? node.height : 34; break;
      case M4NativeUi::NodeType::Spacer: fixedHeight += std::max(0, node.height); break;
      case M4NativeUi::NodeType::Divider:
        fixedHeight += node.height > 0 ? node.height : (M4NativeUi::hasStyle(node.style, M4NativeUi::StyleHairline) ? 6 : 12);
        break;
      case M4NativeUi::NodeType::List: if (!flexList) flexList = &node; break;
      case M4NativeUi::NodeType::Buttons: break;
    }
  }
  const bool hasSubtitle = flexList && !flexList->subtitleField.empty();
  const int baseRowHeight = hasSubtitle ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight;
  const int rowHeight = flexList ? M4UiText::listRowHeight(renderer, UI_10_FONT_ID, baseRowHeight, hasSubtitle) : baseRowHeight;
  const int flexHeight = flexList ? std::max(rowHeight, contentBottom - y - fixedHeight) : 0;

  for (const auto& node : screen->nodes) {
    if (y >= contentBottom && node.type != M4NativeUi::NodeType::Buttons) break;
    switch (node.type) {
      case M4NativeUi::NodeType::Text: {
        const int height = node.height > 0 ? node.height : textDefaultHeight(node);
        const std::string text = resolved(node.text);
        const bool hero = M4NativeUi::hasStyle(node.style, M4NativeUi::StyleHero);
        const bool section = M4NativeUi::hasStyle(node.style, M4NativeUi::StyleSection);
        const bool muted = M4NativeUi::hasStyle(node.style, M4NativeUi::StyleMuted);
        const bool center = M4NativeUi::hasStyle(node.style, M4NativeUi::StyleCenter);
        const int fontId = (section || muted) ? UI_10_FONT_ID : UI_12_FONT_ID;
        const auto family = (node.bold || hero || section) ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
        const int pad = nodeSidePadding(node, metrics.contentSidePadding);
        if (center) M4UiText::drawCentered(renderer, fontId, y + 4, text.c_str(), true, family);
        else if (muted) M4UiText::drawMuted(renderer, fontId, pad, y + 4, text.c_str(), family);
        else M4UiText::draw(renderer, fontId, pad, y + 4, text.c_str(), true, family);
        if (section) renderer.fillRect(pad, y + height - 2, std::max(1, w - 2 * pad), 1, true);
        y += height;
        break;
      }
      case M4NativeUi::NodeType::FlowText: {
        const int height = node.height > 0 ? node.height : flowDefaultHeight(node);
        const std::string text = resolved(node.text.empty() ? node.source : node.text);
        const bool muted = M4NativeUi::hasStyle(node.style, M4NativeUi::StyleMuted);
        const bool compact = M4NativeUi::hasStyle(node.style, M4NativeUi::StyleCompact) || muted;
        const int fontId = compact ? UI_10_FONT_ID : UI_12_FONT_ID;
        const int lineStep = compact ? 24 : 28;
        const int pad = nodeSidePadding(node, metrics.contentSidePadding);
        size_t start = 0;
        int lineY = y + 2;
        while (start <= text.size() && lineY < y + height) {
          const size_t nl = text.find('\n', start);
          const size_t end = nl == std::string::npos ? text.size() : nl;
          const std::string logical = text.substr(start, end - start);
          const std::string line = M4UiText::truncated(renderer, fontId, logical.c_str(), w - 2 * pad);
          if (muted) M4UiText::drawMuted(renderer, fontId, pad, lineY, line.c_str());
          else M4UiText::draw(renderer, fontId, pad, lineY, line.c_str());
          lineY += lineStep;
          if (nl == std::string::npos) break;
          start = nl + 1;
        }
        y += height;
        break;
      }
      case M4NativeUi::NodeType::Tiles: {
        const int height = node.height > 0 ? node.height : tilesDefaultHeight(node);
        const size_t requested = node.pageSize > 0 ? static_cast<size_t>(node.pageSize) : 8u;
        const int count = static_cast<int>(std::min<size_t>(8, std::min(requested, controller_->rowCount(node.source))));
        const int pad = nodeSidePadding(node, metrics.contentSidePadding);
        constexpr int gap = 6;
        constexpr int columns = 4;
        const int rows = std::max(1, (count + columns - 1) / columns);
        const int innerWidth = std::max(1, w - 2 * pad);
        const int cellWidth = std::max(1, (innerWidth - gap * (columns - 1)) / columns);
        const int cellHeight = std::max(1, (height - gap * (rows - 1)) / rows);
        tilesTop_ = y;
        tilesHeight_ = height;
        tilesCount_ = count;
        tilesColumns_ = columns;
        tilesNode_ = &node;
        for (int i = 0; i < count; ++i) {
          M4NativeUi::Row row;
          if (!controller_->rowAt(node.source, static_cast<size_t>(i), row)) continue;
          const int col = i % columns;
          const int r = i / columns;
          const int x = pad + col * (cellWidth + gap);
          const int tileY = y + r * (cellHeight + gap);
          const bool selected = row.value == "selected";
          if (selected && cellWidth > 8 && cellHeight > 8) renderer.fillRoundedRect(x + 3, tileY + 3, cellWidth - 6, cellHeight - 6, 7, Color::LightGray);
          const std::string titleText = M4UiText::truncated(renderer, UI_10_FONT_ID, row.title.c_str(), std::max(1, cellWidth - 8), selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
          M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, x, tileY, cellWidth, cellHeight, titleText.c_str(), true, selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR, 4);
        }
        y += height;
        break;
      }
      case M4NativeUi::NodeType::Tabs: {
        const int height = node.height > 0 ? node.height : metrics.tabBarHeight;
        const size_t count = std::min<size_t>(12, controller_->rowCount(node.source));
        std::vector<M4NativeUi::Row> rows(count);
        std::vector<TabInfo> tabs;
        tabs.reserve(count);
        for (size_t i = 0; i < count; ++i) {
          controller_->rowAt(node.source, i, rows[i]);
          tabs.push_back({rows[i].title.c_str(), static_cast<int>(i) == tabIndex_});
        }
        GUI.drawTabBar(renderer, Rect{0, y, w, height}, tabs, true);
        y += height;
        break;
      }
      case M4NativeUi::NodeType::List: {
        if (&node != flexList) break;
        listTop_ = y;
        listHeight_ = flexHeight;
        listSource_ = node.source;
        listNodeId_ = node.id;
        listAction_ = node.action;
        listCount_ = static_cast<int>(std::min<size_t>(200000, controller_->rowCount(node.source)));
        if (listCount_ <= 0) {
          M4UiText::drawCentered(renderer, UI_10_FONT_ID, y + 40, "暂无内容");
        } else {
          selectedIndex_ = std::max(0, std::min(selectedIndex_, listCount_ - 1));
          int cachedIndex = -1;
          M4NativeUi::Row cachedRow;
          auto row = [&](int index) -> const M4NativeUi::Row& {
            if (cachedIndex != index) {
              cachedRow = {};
              controller_->rowAt(node.source, static_cast<size_t>(index), cachedRow);
              cachedIndex = index;
            }
            return cachedRow;
          };
          const bool ranked = M4NativeUi::hasStyle(node.style, M4NativeUi::StyleRanked);
          const std::function<std::string(int)> titleFn = [&](int i) { return ranked ? rankedTitle(i, row(i).title) : row(i).title; };
          const std::function<std::string(int)> subFn = node.subtitleField.empty() ? std::function<std::string(int)>() : std::function<std::string(int)>([&](int i) { return row(i).subtitle; });
          const std::function<UIIcon(int)> iconFn;
          const std::function<std::string(int)> valueFn = [&](int i) { return row(i).value; };
          const int pad = nodeSidePadding(node, 0);
          GUI.drawList(renderer, Rect{pad, y, std::max(1, w - 2 * pad), flexHeight}, listCount_, selectedIndex_, titleFn, subFn, iconFn, valueFn);
        }
        y += flexHeight;
        break;
      }
      case M4NativeUi::NodeType::Progress: {
        const int height = node.height > 0 ? node.height : 34;
        int value = node.value;
        const int maximum = node.max > 0 ? node.max : 100;
        if (M4NativeUi::isBinding(node.source)) {
          int bound = value;
          if (controller_->number(node.source.substr(1), bound)) value = bound;
        }
        value = std::max(0, std::min(maximum, value));
        const int pad = nodeSidePadding(node, metrics.contentSidePadding);
        GUI.drawProgressBar(renderer, Rect{pad, y + 10, w - 2 * pad, 10}, static_cast<size_t>(value), static_cast<size_t>(std::max(1, maximum)));
        y += height;
        break;
      }
      case M4NativeUi::NodeType::Spacer: y += std::max(0, node.height); break;
      case M4NativeUi::NodeType::Divider: {
        const int pad = nodeSidePadding(node, metrics.contentSidePadding);
        const int height = node.height > 0 ? node.height : (M4NativeUi::hasStyle(node.style, M4NativeUi::StyleHairline) ? 6 : 12);
        renderer.fillRect(pad, y + std::max(1, height / 2), std::max(1, w - 2 * pad), 1, true);
        y += height;
        break;
      }
      case M4NativeUi::NodeType::Buttons:
        for (int i = 0; i < 4; ++i) buttonActions_[i] = node.actions[i];
        break;
    }
  }

  const M4NativeUi::Node* buttons = nullptr;
  for (const auto& node : screen->nodes) {
    if (node.type == M4NativeUi::NodeType::Buttons) { buttons = &node; break; }
  }
  const char* raw[4] = {"« 返回", listCount_ > 0 ? "打开" : "", "", ""};
  std::string owned[4];
  if (buttons) {
    for (int i = 0; i < 4; ++i) {
      owned[i] = resolved(buttons->labels[i]);
      if (!owned[i].empty()) raw[i] = owned[i].c_str();
    }
  }
  const auto labels = mappedInput.mapLabels(raw[0], raw[1], raw[2], raw[3]);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

std::string NativeAppActivity::debugUiJson() {
  if (subActivity) return subActivity->debugUiJson();
  return "{\"kind\":\"native_app\",\"app_id\":\"" + jsonEscape(app_.id) +
         "\",\"provider\":\"" + jsonEscape(app_.provider) + "\",\"screen\":\"" +
         jsonEscape(screenId_) + "\",\"selected\":" + std::to_string(selectedIndex_) +
         ",\"rows\":" + std::to_string(listCount_) + ",\"error\":\"" + jsonEscape(error_) + "\"}";
}
