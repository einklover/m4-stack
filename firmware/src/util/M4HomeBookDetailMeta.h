#pragma once

// Home-page book-detail metadata (pure, SD-free, host-testable).
//
// Visible fields: title, author, plugin display name, plus the existing
// graphical progress bar value. Textual progress ("37%", "已读…") is not
// part of this contract. New provider history stores the real author;
// legacy provider rows may still carry an appId in author.

#include "util/M4ContentProviderContract.h"

#include <cctype>
#include <string>
#include <vector>

namespace M4HomeBookDetailMeta {

struct Labels {
  const char* emptyAuthor = "无";
  const char* localSource = "本地";
  const char* unknownSource = "未知来源";
};

struct InstalledPlugin {
  std::string id;        // package id, e.g. com.weread.client
  std::string name;      // user-facing plugin display name
  std::string provider;  // provider key, e.g. weread
};

struct Presented {
  std::string title;
  std::string author;
  std::string source;
  int progress = 0;  // 0-100 for the graphical bar; never a text field
};

// Same rule as M4xIsValidPackageId: reverse-DNS-ish [a-z0-9_.-] with a dot.
inline bool looksLikePackageId(const std::string& id) {
  if (id.size() < 3 || id.size() > 64) return false;
  bool hasDot = false;
  for (size_t i = 0; i < id.size(); ++i) {
    const char c = id[i];
    if (c == '.') {
      hasDot = true;
      if (i == 0 || i + 1 == id.size()) return false;
      continue;
    }
    if (!(std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '_' ||
          c == '-')) {
      return false;
    }
  }
  return hasDot;
}

inline bool isUsableDisplayName(const std::string& name, const std::string& id, const std::string& provider) {
  if (name.empty()) return false;
  if (name == id) return false;
  if (looksLikePackageId(name)) return false;
  if (!provider.empty() && name == provider) return false;
  return true;
}

// Manifest `name` values for the built-in novel plugins. Used only when the
// installed-plugin registry is missing or has no usable display name.
inline const char* builtinAppDisplayName(const std::string& appId) {
  if (appId == "com.weread.client") return "微信读书";
  if (appId == "com.fanqie.client") return "番茄小说";
  if (appId == "com.jjwxc.client") return "晋江文学";
  if (appId == "com.legado.client") return "开源阅读";
  return nullptr;
}

inline const char* builtinProviderDisplayName(const std::string& providerId) {
  if (providerId == "weread") return "微信读书";
  if (providerId == "fanqie") return "番茄小说";
  if (providerId == "jjwxc") return "晋江文学";
  if (providerId == "legado") return "开源阅读";
  return nullptr;
}

inline std::string orLabel(const char* value, const char* fallback) {
  if (value && value[0]) return value;
  return fallback ? fallback : "";
}

inline std::string resolvePluginDisplayName(const std::string& providerId, const std::string& appId,
                                            const std::vector<InstalledPlugin>& plugins, const Labels& labels) {
  for (const auto& plugin : plugins) {
    if (!appId.empty() && plugin.id == appId && isUsableDisplayName(plugin.name, plugin.id, plugin.provider)) {
      return plugin.name;
    }
  }
  for (const auto& plugin : plugins) {
    if (!providerId.empty() && plugin.provider == providerId &&
        isUsableDisplayName(plugin.name, plugin.id, plugin.provider)) {
      return plugin.name;
    }
  }
  if (const char* name = builtinAppDisplayName(appId)) return name;
  if (const char* name = builtinProviderDisplayName(providerId)) return name;
  return orLabel(labels.unknownSource, "未知来源");
}

inline std::vector<InstalledPlugin>& installedPlugins() {
  static std::vector<InstalledPlugin> plugins;
  return plugins;
}

inline void setInstalledPlugins(std::vector<InstalledPlugin> plugins) { installedPlugins() = std::move(plugins); }

inline Presented present(const std::string& path, const std::string& title, const std::string& authorField,
                         int progress, const std::vector<InstalledPlugin>& plugins, const Labels& labels = {}) {
  Presented out;
  out.title = title;
  out.progress = progress;

  std::string providerId;
  std::string bookId;
  const bool pluginHistory = M4ContentProvider::parseHistoryUri(path.c_str(), providerId, bookId);
  // A local book may have any author string. Only URI-backed history has the
  // legacy appId interpretation.
  const bool authorIsAppId = pluginHistory && looksLikePackageId(authorField);
  if (authorField.empty() || authorIsAppId) {
    out.author = orLabel(labels.emptyAuthor, "无");
  } else {
    out.author = authorField;
  }

  if (pluginHistory || authorIsAppId) {
    out.source = resolvePluginDisplayName(providerId, authorIsAppId ? authorField : std::string(), plugins, labels);
  } else {
    out.source = orLabel(labels.localSource, "本地");
  }
  return out;
}

inline Presented presentCached(const std::string& path, const std::string& title, const std::string& authorField,
                               int progress, const Labels& labels = {}) {
  return present(path, title, authorField, progress, installedPlugins(), labels);
}

}  // namespace M4HomeBookDetailMeta
