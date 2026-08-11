#include "apps/providers/M4NativeProviderIo.h"

#include <ArduinoJson.h>
#include <SDCardManager.h>

#include <algorithm>

namespace M4NativeProviderIo {
namespace {

bool readCredentialFile(const std::string& path, std::string& out) {
  out.clear();
  FsFile f;
  if (!SdMan.openFileForRead("NP-AUTH", path.c_str(), f)) return false;
  const size_t n = f.fileSize();
  if (n == 0 || n > 16u * 1024u) {
    f.close();
    return false;
  }
  out.resize(n);
  size_t off = 0;
  while (off < n) {
    const int r = f.read(reinterpret_cast<uint8_t*>(&out[off]), n - off);
    if (r <= 0) break;
    off += static_cast<size_t>(r);
  }
  f.close();
  if (off != n) {
    out.clear();
    return false;
  }
  return true;
}

bool writeCredentialFile(const std::string& path, const std::string& body) {
  const std::string tmp = path + ".tmp";
  if (!M4NativeProviderIo::ensureParentDirs(path)) return false;
  if (SdMan.exists(tmp.c_str())) SdMan.remove(tmp.c_str());
  FsFile f;
  if (!SdMan.openFileForWrite("NP-AUTH", tmp.c_str(), f)) return false;
  size_t off = 0;
  while (off < body.size()) {
    const size_t want = std::min<size_t>(4096, body.size() - off);
    const int n = f.write(reinterpret_cast<const uint8_t*>(body.data() + off), want);
    if (n <= 0) {
      f.close();
      SdMan.remove(tmp.c_str());
      return false;
    }
    off += static_cast<size_t>(n);
  }
  f.close();
  if (SdMan.exists(path.c_str())) SdMan.remove(path.c_str());
  if (!SdMan.rename(tmp.c_str(), path.c_str())) {
    SdMan.remove(tmp.c_str());
    return false;
  }
  return true;
}

}  // namespace

bool storeCookieValues(const std::string& appDataRoot,
                       const std::vector<std::pair<std::string, std::string>>& values) {
  if (appDataRoot.empty() || values.empty()) return false;
  const std::string path = appDataRoot + (appDataRoot.back() == '/' ? "config.json" : "/config.json");
  std::string raw;
  JsonDocument doc;
  if (readCredentialFile(path, raw)) (void)deserializeJson(doc, raw);
  JsonObject cookies = doc["cookies"].is<JsonObject>() ? doc["cookies"].as<JsonObject>()
                                                       : doc["cookies"].to<JsonObject>();
  bool changed = false;
  for (const auto& kv : values) {
    if (kv.first.empty() || kv.second.empty()) continue;
    cookies[kv.first] = kv.second;
    // Keep the historical top-level fields that Paper S3 and earlier M4
    // WeRead packages also understand. This is compatibility, not duplication
    // into history/provider metadata.
    if (kv.first == "wr_vid" || kv.first == "wr_skey" || kv.first == "wr_rt") {
      doc[kv.first] = kv.second;
    }
    changed = true;
  }
  if (!changed) return false;
  std::string out;
  serializeJson(doc, out);
  return out.size() <= 16u * 1024u && writeCredentialFile(path, out);
}

}  // namespace M4NativeProviderIo
