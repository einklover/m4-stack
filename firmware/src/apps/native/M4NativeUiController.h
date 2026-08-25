#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace M4NativeUi {

struct Row {
  std::string key;
  std::string title;
  std::string subtitle;
  std::string value;
  std::string coverUrl;
  bool enabled = true;
};

struct ActionContext {
  std::string screenId;
  std::string nodeId;
  std::string source;
  std::string rowKey;
  int index0 = -1;
};

enum class ActionKind {
  None = 0,
  Repaint,
  Navigate,
  Close,
  OpenProviderBook,
  OpenProviderToc,
  OpenLogin,
  OpenEndpoint,
  OpenScreenBridge,
  Error,
};

struct ActionResult {
  ActionKind kind = ActionKind::None;
  std::string screenId;
  std::string payload;  // route payload, e.g. m4cp://provider/book
  std::string error;

  static ActionResult repaint() {
    ActionResult r;
    r.kind = ActionKind::Repaint;
    return r;
  }
  static ActionResult navigate(std::string screen) {
    ActionResult r;
    r.kind = ActionKind::Navigate;
    r.screenId = std::move(screen);
    return r;
  }
  static ActionResult close() {
    ActionResult r;
    r.kind = ActionKind::Close;
    return r;
  }
  static ActionResult openProviderBook(std::string uri) {
    ActionResult r;
    r.kind = ActionKind::OpenProviderBook;
    r.payload = std::move(uri);
    return r;
  }
  static ActionResult openScreenBridge() {
    ActionResult r;
    r.kind = ActionKind::OpenScreenBridge;
    return r;
  }
  static ActionResult openEndpoint() {
    ActionResult r;
    r.kind = ActionKind::OpenEndpoint;
    return r;
  }
};

// Native UI never evaluates code from XML. It asks a native controller for
// scalar values/rows and dispatches a named action from a strict allow-list
// implemented by that controller/provider.
class Controller {
 public:
  virtual ~Controller() = default;

  virtual bool scalar(const std::string& key, std::string& out) const {
    (void)key;
    out.clear();
    return false;
  }
  virtual bool number(const std::string& key, int& out) const {
    (void)key;
    out = 0;
    return false;
  }
  virtual size_t rowCount(const std::string& source) const {
    (void)source;
    return 0;
  }
  virtual bool rowAt(const std::string& source, size_t index0, Row& out) const {
    (void)source;
    (void)index0;
    out = {};
    return false;
  }
  virtual ActionResult dispatch(const std::string& action, const ActionContext& ctx) {
    (void)action;
    (void)ctx;
    return {};
  }

  // Monotonic-ish state token for asynchronous controller data. The activity
  // only compares equality; wraparound is harmless. A changed token requests
  // one repaint, avoiding timers or network work inside render().
  virtual uint32_t revision() const { return 0; }

  // Called once per activity loop frame (outside render). Providers may start
  // background discovery/network here; must not block.
  virtual void pollAsync() {}
};

inline bool isBinding(const std::string& s) { return s.size() > 1 && s[0] == '@'; }

inline std::string resolveText(const Controller& c, const std::string& text) {
  if (!isBinding(text)) return text;
  std::string out;
  if (c.scalar(text.substr(1), out)) return out;
  return {};
}

}  // namespace M4NativeUi
