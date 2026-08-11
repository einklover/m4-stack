#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace M4NativeLoadUi {

enum class Scope { Discovery = 0, Catalog, Chapter };
enum class Stage { Preparing = 0, Resolving, Connecting, Receiving, Processing, Writing, Ready, AuthRequired, Error, Cancelled };

struct Snapshot {
  Scope scope = Scope::Discovery;
  Stage stage = Stage::Preparing;
  size_t receivedBytes = 0;
  size_t writtenBytes = 0;
  size_t rows = 0;
  int percent = 0;
  uint32_t elapsedSeconds = 0;
  std::string error;
};

inline const char* objectName(Scope scope) {
  switch (scope) {
    case Scope::Catalog: return "目录";
    case Scope::Chapter: return "正文";
    case Scope::Discovery:
    default: return "书库";
  }
}

inline std::string title(const Snapshot& s) {
  const char* object = objectName(s.scope);
  switch (s.stage) {
    case Stage::Resolving: return std::string("解析") + object;
    case Stage::Connecting: return std::string("连接") + object;
    case Stage::Receiving: return std::string("接收") + object;
    case Stage::Processing: return std::string("整理") + object;
    case Stage::Writing: return std::string("保存") + object;
    case Stage::Ready: return std::string(object) + "就绪";
    case Stage::AuthRequired: return "需要登录";
    case Stage::Error: return std::string(object) + "加载失败";
    case Stage::Cancelled: return "已取消";
    case Stage::Preparing:
    default: return std::string("准备") + object;
  }
}

inline std::string detail(const Snapshot& s) {
  char buf[144];
  if (s.rows > 0) {
    std::snprintf(buf, sizeof(buf), "%u 项 · %u KB · %us",
                  static_cast<unsigned>(s.rows),
                  static_cast<unsigned>(s.receivedBytes / 1024u),
                  static_cast<unsigned>(s.elapsedSeconds));
    return buf;
  }
  if (s.receivedBytes || s.writtenBytes) {
    if (s.writtenBytes) {
      std::snprintf(buf, sizeof(buf), "接收 %u KB · 写入 %u KB · %us",
                    static_cast<unsigned>(s.receivedBytes / 1024u),
                    static_cast<unsigned>(s.writtenBytes / 1024u),
                    static_cast<unsigned>(s.elapsedSeconds));
    } else {
      std::snprintf(buf, sizeof(buf), "接收 %u KB · %us",
                    static_cast<unsigned>(s.receivedBytes / 1024u),
                    static_cast<unsigned>(s.elapsedSeconds));
    }
    return buf;
  }
  if (s.percent > 0 && s.percent < 100) {
    std::snprintf(buf, sizeof(buf), "%d%% · %us", s.percent,
                  static_cast<unsigned>(s.elapsedSeconds));
    return buf;
  }
  std::snprintf(buf, sizeof(buf), "已用时 %u 秒", static_cast<unsigned>(s.elapsedSeconds));
  return buf;
}

// Stable machine-readable values for plugin/debug surfaces. Provider code may
// expose these keys without inventing site-specific loading strings.
inline const char* stageKey(Stage stage) {
  switch (stage) {
    case Stage::Resolving: return "resolving";
    case Stage::Connecting: return "connecting";
    case Stage::Receiving: return "receiving";
    case Stage::Processing: return "processing";
    case Stage::Writing: return "writing";
    case Stage::Ready: return "ready";
    case Stage::AuthRequired: return "auth_required";
    case Stage::Error: return "error";
    case Stage::Cancelled: return "cancelled";
    case Stage::Preparing:
    default: return "preparing";
  }
}

}  // namespace M4NativeLoadUi
