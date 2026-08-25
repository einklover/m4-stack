// Host contract for home-page book-detail metadata.
// Pins: plugin display name (never raw id), no textual progress, progress bar
// value unchanged, local/missing source render safely.
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "util/M4HomeBookDetailMeta.h"

using namespace M4HomeBookDetailMeta;

namespace {

const Labels kLabels{"无", "本地", "未知来源"};

void plugin_display_name_not_raw_id() {
  const std::vector<InstalledPlugin> plugins = {
      {"com.weread.client", "微信读书", "weread"},
  };
  const auto meta = present("m4cp://weread/book123", "三体", "com.weread.client", 37, plugins, kLabels);
  assert(meta.title == "三体");
  assert(meta.author == "无");  // author field is appId; do not leak it
  assert(meta.source == "微信读书");
  assert(meta.source.find("com.weread.client") == std::string::npos);
  assert(meta.source.find("weread") == std::string::npos);
  assert(meta.progress == 37);
  std::cout << "plugin display name OK\n";
}

void registry_name_wins_over_builtin() {
  const std::vector<InstalledPlugin> plugins = {
      {"com.weread.client", "微信读书（测试包）", "weread"},
  };
  const auto meta = present("m4cp://weread/x", "T", "com.weread.client", 10, plugins, kLabels);
  assert(meta.source == "微信读书（测试包）");
  std::cout << "registry display name OK\n";
}

void builtin_fallback_when_registry_empty() {
  const auto meta = present("m4cp://fanqie/b1", "书", "com.fanqie.client", 0, {}, kLabels);
  assert(meta.source == "番茄小说");
  assert(meta.source.find("com.fanqie") == std::string::npos);
  assert(meta.source != "fanqie");
  std::cout << "builtin plugin name fallback OK\n";
}

void jjwxc_and_legado_builtin_names() {
  const auto jj = present("m4cp://jjwxc/1", "A", "com.jjwxc.client", 1, {}, kLabels);
  assert(jj.source == "晋江文学");
  const auto lg = present("m4cp://legado/2", "B", "com.legado.client", 2, {}, kLabels);
  assert(lg.source == "开源阅读");
  std::cout << "jjwxc/legado builtin names OK\n";
}

void registry_name_that_is_raw_id_is_skipped() {
  const std::vector<InstalledPlugin> plugins = {
      {"com.weread.client", "com.weread.client", "weread"},
  };
  const auto meta = present("m4cp://weread/x", "T", "com.weread.client", 5, plugins, kLabels);
  assert(meta.source == "微信读书");
  assert(meta.source != "com.weread.client");
  std::cout << "raw-id registry name skipped OK\n";
}

void no_textual_progress_in_metadata() {
  const auto meta = present("m4cp://weread/x", "T", "com.weread.client", 64, {}, kLabels);
  assert(meta.progress == 64);
  assert(meta.title.find('%') == std::string::npos);
  assert(meta.author.find('%') == std::string::npos);
  assert(meta.source.find('%') == std::string::npos);
  assert(meta.title.find("已读") == std::string::npos);
  assert(meta.author.find("已读") == std::string::npos);
  assert(meta.source.find("已读") == std::string::npos);
  std::cout << "no textual progress OK\n";
}

void progress_value_passthrough() {
  assert(present("/books/a.epub", "A", "作者", 0, {}, kLabels).progress == 0);
  assert(present("/books/a.epub", "A", "作者", 1, {}, kLabels).progress == 1);
  assert(present("/books/a.epub", "A", "作者", 100, {}, kLabels).progress == 100);
  assert(present("/books/a.epub", "A", "作者", 64, {}, kLabels).progress == 64);
  std::cout << "progress passthrough OK\n";
}

void local_epub_source_and_author() {
  const auto meta = present("/books/local.epub", "本地书", "鲁迅", 12, {}, kLabels);
  assert(meta.title == "本地书");
  assert(meta.author == "鲁迅");
  assert(meta.source == "本地");
  assert(meta.progress == 12);
  std::cout << "local epub OK\n";
}

void local_txt_missing_author() {
  const auto meta = present("/books/note.txt", "note.txt", "", 0, {}, kLabels);
  assert(meta.author == "无");
  assert(meta.source == "本地");
  std::cout << "local missing author OK\n";
}

void unknown_plugin_does_not_leak_id() {
  const auto meta = present("m4cp://obscure/book9", "X", "com.obscure.client", 3, {}, kLabels);
  assert(meta.source == "未知来源");
  assert(meta.source.find("obscure") == std::string::npos);
  assert(meta.author == "无");
  assert(meta.author.find("com.obscure") == std::string::npos);
  std::cout << "unknown source fallback OK\n";
}

void history_uri_without_appid_still_names_source() {
  const auto meta = present("m4cp://weread/book9", "X", "", 8, {}, kLabels);
  assert(meta.source == "微信读书");
  assert(meta.author == "无");
  std::cout << "uri-only source OK\n";
}

void malformed_history_is_local() {
  const auto meta = present("m4cp://", "X", "", 0, {}, kLabels);
  assert(meta.source == "本地");
  std::cout << "malformed uri local OK\n";
}

void installed_plugin_cache() {
  setInstalledPlugins({{"com.weread.client", "微信读书", "weread"}});
  const auto meta = presentCached("m4cp://weread/b", "T", "com.weread.client", 9, kLabels);
  assert(meta.source == "微信读书");
  assert(meta.progress == 9);
  setInstalledPlugins({});
  std::cout << "installed plugin cache OK\n";
}

}  // namespace

int main() {
  plugin_display_name_not_raw_id();
  registry_name_wins_over_builtin();
  builtin_fallback_when_registry_empty();
  jjwxc_and_legado_builtin_names();
  registry_name_that_is_raw_id_is_skipped();
  no_textual_progress_in_metadata();
  progress_value_passthrough();
  local_epub_source_and_author();
  local_txt_missing_author();
  unknown_plugin_does_not_leak_id();
  history_uri_without_appid_still_names_source();
  malformed_history_is_local();
  installed_plugin_cache();
  std::cout << "home book detail meta tests passed\n";
  return 0;
}
