#include "apps/providers/M4NativeProviderIo.h"
#include "apps/M4xNetPolicy.h"

#include <Arduino.h>
#include <ArduinoJson.h>

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace M4NativeProviderIo {
namespace {

std::string markerPath(const std::string& p) { return p + ".ok"; }

bool readSmall(const std::string& path, std::string& out, size_t cap = 16u * 1024u) {
  out.clear();
  FsFile f;
  if (!SdMan.openFileForRead("NP-CFG", path.c_str(), f)) return false;
  const size_t n = f.fileSize();
  if (n == 0 || n > cap) {
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

bool writeExact(const std::string& path, const std::string& body) {
  if (SdMan.exists(path.c_str())) SdMan.remove(path.c_str());
  FsFile f;
  if (!SdMan.openFileForWrite("NP-CFG", path.c_str(), f)) return false;
  size_t off = 0;
  while (off < body.size()) {
    const size_t n = std::min<size_t>(4096, body.size() - off);
    const int w = f.write(reinterpret_cast<const uint8_t*>(body.data() + off), n);
    if (w <= 0) {
      f.close();
      return false;
    }
    off += static_cast<size_t>(w);
  }
  f.sync();
  f.close();
  return true;
}

bool fileSizeIs(const std::string& path, size_t expected) {
  FsFile f;
  if (!SdMan.openFileForRead("NP-VERIFY", path.c_str(), f)) return false;
  const size_t n = f.fileSize();
  f.close();
  return n == expected;
}

bool copyFileExact(const std::string& src, const std::string& dst, size_t expectedBytes) {
  FsFile in;
  if (!SdMan.openFileForRead("NP-COPY-R", src.c_str(), in)) return false;
  if (in.fileSize() != expectedBytes) {
    in.close();
    return false;
  }

  if (SdMan.exists(dst.c_str()) && !SdMan.remove(dst.c_str())) {
    in.close();
    return false;
  }
  FsFile out;
  if (!SdMan.openFileForWrite("NP-COPY-W", dst.c_str(), out)) {
    in.close();
    return false;
  }
  (void)out.seek(0);
  (void)out.truncate(0);

  uint8_t buf[2048];
  size_t copied = 0;
  bool ok = true;
  while (copied < expectedBytes) {
    const size_t want = std::min<size_t>(sizeof(buf), expectedBytes - copied);
    const int r = in.read(buf, want);
    if (r <= 0) {
      ok = false;
      break;
    }
    size_t off = 0;
    while (off < static_cast<size_t>(r)) {
      const int w = out.write(buf + off, static_cast<size_t>(r) - off);
      if (w <= 0) {
        ok = false;
        break;
      }
      off += static_cast<size_t>(w);
    }
    if (!ok) break;
    copied += static_cast<size_t>(r);
  }
  out.sync();
  out.close();
  in.close();

  if (!ok || copied != expectedBytes || !fileSizeIs(dst, expectedBytes)) {
    if (SdMan.exists(dst.c_str())) SdMan.remove(dst.c_str());
    return false;
  }
  return true;
}

std::string configPath(const std::string& root) {
  if (root.empty()) return {};
  return root.back() == '/' ? root + "config.json" : root + "/config.json";
}

std::string trim(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
  return s;
}

}  // namespace

std::string replacedExtension(const std::string& path, const char* ext) {
  if (path.empty() || ext == nullptr || ext[0] == 0) return path;
  const size_t slash = path.find_last_of('/');
  const size_t dot = path.find_last_of('.');
  const size_t stemEnd =
      (dot != std::string::npos && (slash == std::string::npos || dot > slash)) ? dot : path.size();
  std::string out = path.substr(0, stemEnd);
  if (ext[0] != '.') out.push_back('.');
  out += ext;
  return out;
}

bool ensureParentDirs(const std::string& absPath) {
  const size_t slash = absPath.find_last_of('/');
  if (slash == std::string::npos || slash == 0) return true;
  const std::string dir = absPath.substr(0, slash);
  size_t p = 1;
  while (p <= dir.size()) {
    const size_t next = dir.find('/', p);
    const std::string sub = dir.substr(0, next == std::string::npos ? dir.size() : next);
    if (!sub.empty()) {
      // A zero-byte regular file can block the path (seen as apps_data/<id>/provider
      // on QEMU SD). exists() is true so mkdir was skipped, and later open of
      // provider/shelf_rows.tsv fails with sd_open_failed — empty native home.
      if (SdMan.exists(sub.c_str())) {
        FsFile probe = SdMan.open(sub.c_str(), O_RDONLY);
        const bool isDir = probe && probe.isDirectory();
        if (probe) probe.close();
        if (!isDir) {
          // Directory open can fail transiently; if remove also fails, still try mkdir.
          (void)SdMan.remove(sub.c_str());
          if (!SdMan.ensureDirectoryExists(sub.c_str()) && !SdMan.mkdir(sub.c_str(), true)) {
            // Re-probe: another task may have created the directory.
            FsFile again = SdMan.open(sub.c_str(), O_RDONLY);
            const bool ok = again && again.isDirectory();
            if (again) again.close();
            if (!ok) return false;
          }
        }
      } else if (!SdMan.ensureDirectoryExists(sub.c_str()) && !SdMan.mkdir(sub.c_str(), true)) {
        return false;
      }
    }
    if (next == std::string::npos) break;
    p = next + 1;
  }
  if (!SdMan.exists(dir.c_str())) return false;
  FsFile finalProbe = SdMan.open(dir.c_str(), O_RDONLY);
  if (finalProbe && finalProbe.isDirectory()) {
    finalProbe.close();
    return true;
  }
  if (finalProbe) finalProbe.close();
  // Last resort: treat ensureDirectoryExists success as enough even if open/isDirectory
  // is flaky on some FAT mounts (QEMU SD).
  return SdMan.ensureDirectoryExists(dir.c_str());
}

bool cacheComplete(const std::string& absPath, size_t* sizeOut) {
  if (sizeOut) *sizeOut = 0;
  if (absPath.empty() || !SdMan.exists(absPath.c_str()) || !SdMan.exists(markerPath(absPath).c_str())) return false;
  FsFile f;
  if (!SdMan.openFileForRead("NP-CACHE", absPath.c_str(), f)) return false;
  const size_t n = f.fileSize();
  f.close();
  if (sizeOut) *sizeOut = n;
  return n > 0;
}

bool cacheVerified(const std::string& absPath, size_t* sizeOut) {
  size_t actual = 0;
  if (!cacheComplete(absPath, &actual)) return false;
  std::string marker;
  if (!readSmall(markerPath(absPath), marker, 64) || marker.rfind("v2:", 0) != 0) return false;
  char* end = nullptr;
  const unsigned long long expected = std::strtoull(marker.c_str() + 3, &end, 10);
  while (end && *end && std::isspace(static_cast<unsigned char>(*end))) ++end;
  if (!end || *end || expected != actual) return false;
  if (sizeOut) *sizeOut = actual;
  return true;
}

bool readSmallText(const std::string& path, std::string& out, size_t cap) {
  return readSmall(path, out, cap);
}

bool writeTextFile(const std::string& path, const std::string& body) {
  if (path.empty() || body.empty() || !ensureParentDirs(path)) return false;
  if (SdMan.exists(path.c_str())) SdMan.remove(path.c_str());
  FsFile f;
  if (!SdMan.openFileForWrite("NP-TEXT", path.c_str(), f)) return false;
  size_t off = 0;
  bool ok = true;
  while (off < body.size()) {
    const size_t n = std::min<size_t>(4096, body.size() - off);
    const int w = f.write(reinterpret_cast<const uint8_t*>(body.data() + off), n);
    if (w != static_cast<int>(n)) {
      ok = false;
      break;
    }
    off += n;
  }
  if (ok) f.flush();
  f.close();
  if (!ok && SdMan.exists(path.c_str())) SdMan.remove(path.c_str());
  return ok;
}

bool commitTempFilesPair(const std::string& firstTemp, const std::string& firstFinal,
                         size_t firstBytes, const std::string& secondTemp,
                         const std::string& secondFinal, size_t secondBytes) {
  if (firstTemp.empty() || firstFinal.empty() || secondTemp.empty() || secondFinal.empty() ||
      firstTemp == firstFinal || secondTemp == secondFinal || secondBytes == 0 ||
      !SdMan.exists(firstTemp.c_str()) || !SdMan.exists(secondTemp.c_str()) ||
      !fileSizeIs(firstTemp, firstBytes) || !fileSizeIs(secondTemp, secondBytes) ||
      !ensureParentDirs(firstFinal) || !ensureParentDirs(secondFinal)) {
    return false;
  }

  const std::string firstBackup = replacedExtension(firstFinal, "rkb");
  const std::string secondBackup = replacedExtension(secondFinal, "mkb");
  if (firstBackup == secondBackup || firstBackup == firstFinal || secondBackup == secondFinal) {
    return false;
  }
  if (SdMan.exists(firstBackup.c_str())) SdMan.remove(firstBackup.c_str());
  if (SdMan.exists(secondBackup.c_str())) SdMan.remove(secondBackup.c_str());

  const bool hadFirst = SdMan.exists(firstFinal.c_str());
  const bool hadSecond = SdMan.exists(secondFinal.c_str());
  bool firstBacked = false;
  bool secondBacked = false;
  bool firstInstalled = false;
  bool secondInstalled = false;
  auto restore = [&]() {
    if (firstInstalled && SdMan.exists(firstFinal.c_str())) SdMan.remove(firstFinal.c_str());
    if (secondInstalled && SdMan.exists(secondFinal.c_str())) SdMan.remove(secondFinal.c_str());
    if (firstBacked && SdMan.exists(firstBackup.c_str())) {
      (void)SdMan.rename(firstBackup.c_str(), firstFinal.c_str());
    }
    if (secondBacked && SdMan.exists(secondBackup.c_str())) {
      (void)SdMan.rename(secondBackup.c_str(), secondFinal.c_str());
    }
  };

  if (hadFirst) {
    firstBacked = SdMan.rename(firstFinal.c_str(), firstBackup.c_str());
    if (!firstBacked) return false;
  }
  if (hadSecond) {
    secondBacked = SdMan.rename(secondFinal.c_str(), secondBackup.c_str());
    if (!secondBacked) {
      restore();
      return false;
    }
  }

  bool committed = SdMan.rename(firstTemp.c_str(), firstFinal.c_str());
  firstInstalled = committed;
  committed = committed && fileSizeIs(firstFinal, firstBytes);
  if (committed) {
    committed = SdMan.rename(secondTemp.c_str(), secondFinal.c_str());
    secondInstalled = committed;
    committed = committed && fileSizeIs(secondFinal, secondBytes);
  }
  if (!committed) {
    restore();
    return false;
  }

  if (firstBacked && SdMan.exists(firstBackup.c_str())) SdMan.remove(firstBackup.c_str());
  if (secondBacked && SdMan.exists(secondBackup.c_str())) SdMan.remove(secondBackup.c_str());
  return true;
}

void recoverTempFilesPair(const std::string& firstFinal, const std::string& secondFinal) {
  const std::string firstBackup = replacedExtension(firstFinal, "rkb");
  const std::string secondBackup = replacedExtension(secondFinal, "mkb");
  const bool firstBacked = SdMan.exists(firstBackup.c_str());
  const bool secondBacked = SdMan.exists(secondBackup.c_str());
  if (!firstBacked && !secondBacked) return;

  const bool firstFinalPresent = SdMan.exists(firstFinal.c_str());
  const bool secondFinalPresent = SdMan.exists(secondFinal.c_str());
  if (firstFinalPresent && secondFinalPresent) {
    // Both new finals were installed; only cleanup was interrupted.
    if (firstBacked) SdMan.remove(firstBackup.c_str());
    if (secondBacked) SdMan.remove(secondBackup.c_str());
    return;
  }

  // At least one final is absent, so restore each available old generation.
  // Never delete an untouched sibling that has no corresponding backup.
  if (firstBacked) {
    if (firstFinalPresent) SdMan.remove(firstFinal.c_str());
    (void)SdMan.rename(firstBackup.c_str(), firstFinal.c_str());
  }
  if (secondBacked) {
    if (secondFinalPresent) SdMan.remove(secondFinal.c_str());
    (void)SdMan.rename(secondBackup.c_str(), secondFinal.c_str());
  }
}

bool removeIncomplete(const std::string& absPath) {
  const std::string part = absPath + ".part";
  if (SdMan.exists(part.c_str())) SdMan.remove(part.c_str());
  return !SdMan.exists(part.c_str());
}

bool clearCacheArtifacts(const std::string& absPath) {
  if (absPath.empty()) return false;
  bool ok = true;
  const char* suffixes[] = {"", ".part", ".ok", ".wap.tmp", ".tidx", ".tidx.tmp", ".tidx.bak"};
  for (const char* suffix : suffixes) {
    const std::string path = absPath + suffix;
    if (SdMan.exists(path.c_str()) && !SdMan.remove(path.c_str())) ok = false;
  }
  return ok;
}

bool commitTempFile(const std::string& tempAbsPath, const std::string& finalAbsPath,
                    size_t expectedBytes, bool preserveOld, bool allowAlreadyFinal) {
  // Same 8.3 alias as the live file: the payload is already at the destination.
  if (allowAlreadyFinal && !finalAbsPath.empty() && expectedBytes > 0 &&
      fileSizeIs(finalAbsPath, expectedBytes)) {
    Serial.printf("[NP-IO] commit already-final size=%u %s\n", static_cast<unsigned>(expectedBytes),
                  finalAbsPath.c_str());
    return true;
  }

  if (tempAbsPath.empty() || finalAbsPath.empty() || tempAbsPath == finalAbsPath || expectedBytes == 0) {
    Serial.printf("[NP-IO] commit bad-args expect=%u\n", static_cast<unsigned>(expectedBytes));
    return false;
  }
  if (!SdMan.exists(tempAbsPath.c_str()) || !fileSizeIs(tempAbsPath, expectedBytes)) {
    Serial.printf("[NP-IO] commit tmp-size-mismatch expect=%u tmp=%s\n",
                  static_cast<unsigned>(expectedBytes), tempAbsPath.c_str());
    return false;
  }
  if (!ensureParentDirs(finalAbsPath)) {
    Serial.printf("[NP-IO] commit parent-fail %s\n", finalAbsPath.c_str());
    return false;
  }

  const std::string backup = replacedExtension(finalAbsPath, "bak");
  const bool hadOld = SdMan.exists(finalAbsPath.c_str());
  bool backedUp = false;

  if (SdMan.exists(backup.c_str())) SdMan.remove(backup.c_str());
  if (hadOld) {
    if (preserveOld) {
      if (SdMan.rename(finalAbsPath.c_str(), backup.c_str())) {
        backedUp = true;
      } else if (!SdMan.remove(finalAbsPath.c_str())) {
        Serial.printf("[NP-IO] commit old-busy %s\n", finalAbsPath.c_str());
        return false;
      }
    } else if (!SdMan.remove(finalAbsPath.c_str())) {
      Serial.printf("[NP-IO] commit old-remove %s\n", finalAbsPath.c_str());
      return false;
    }
  }

  bool committed = SdMan.rename(tempAbsPath.c_str(), finalAbsPath.c_str());
  if (committed) committed = fileSizeIs(finalAbsPath, expectedBytes);

  if (!committed) {
    // Some cards/controllers accept the streaming write but fail the metadata
    // rename. The temp file is already complete and closed, so copy it in small
    // bounded chunks and verify the final length before treating it as durable.
    if (SdMan.exists(finalAbsPath.c_str())) SdMan.remove(finalAbsPath.c_str());
    committed = copyFileExact(tempAbsPath, finalAbsPath, expectedBytes);
    if (committed && SdMan.exists(tempAbsPath.c_str())) (void)SdMan.remove(tempAbsPath.c_str());
  }

  if (committed) {
    if (backedUp && SdMan.exists(backup.c_str())) SdMan.remove(backup.c_str());
    return true;
  }

  Serial.printf("[NP-IO] commit rename+copy failed expect=%u %s\n", static_cast<unsigned>(expectedBytes),
                finalAbsPath.c_str());
  if (SdMan.exists(finalAbsPath.c_str())) SdMan.remove(finalAbsPath.c_str());
  if (backedUp && SdMan.exists(backup.c_str())) {
    (void)SdMan.rename(backup.c_str(), finalAbsPath.c_str());
  }
  return false;
}

bool commitPart(const std::string& absPath, size_t* sizeOut) {
  if (sizeOut) *sizeOut = 0;
  const std::string part = absPath + ".part";
  if (!SdMan.exists(part.c_str())) return false;
  FsFile f;
  if (!SdMan.openFileForRead("NP-COMMIT", part.c_str(), f)) return false;
  const size_t n = f.fileSize();
  f.close();
  if (n == 0) return false;

  const std::string ok = markerPath(absPath);
  if (SdMan.exists(ok.c_str())) SdMan.remove(ok.c_str());
  if (!commitTempFile(part, absPath, n, false)) return false;

  if (!writeExact(ok, std::string("v2:") + std::to_string(n) + "\n")) return false;
  if (sizeOut) *sizeOut = n;
  return true;
}

bool PartFileSink::open(const std::string& finalAbsPath) {
  close();
  finalPath_ = finalAbsPath;
  partPath_ = finalPath_ + ".part";
  if (finalPath_.empty() || !ensureParentDirs(finalPath_)) return false;
#if defined(ARDUINO_ARCH_ESP32)
  buffer_ = static_cast<uint8_t*>(heap_caps_malloc(kBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#endif
  if (!buffer_) buffer_ = static_cast<uint8_t*>(std::malloc(kBufferBytes));
  // Defer FatFS open until the first body byte. Holding an SD file open
  // across the TLS handshake races the shared SPI bus with e-ink.
  open_ = true;
  fileReady_ = false;
  return true;
}

bool PartFileSink::ensureFile() {
  if (fileReady_) return true;
  if (!open_ || partPath_.empty()) return false;
  if (SdMan.exists(partPath_.c_str())) SdMan.remove(partPath_.c_str());
  if (!SdMan.openFileForWrite("NP-BODY", partPath_.c_str(), file_)) return false;
  file_.seek(0);
  file_.truncate(0);
  fileReady_ = true;
  return true;
}

bool PartFileSink::flushBuffer() {
  if (!open_ || used_ == 0) return open_;
  if (!ensureFile()) return false;
  const int n = file_.write(buffer_, used_);
  if (n != static_cast<int>(used_)) return false;
  used_ = 0;
  return true;
}

bool PartFileSink::write(const uint8_t* data, size_t len) {
  if (!open_ || !data) return false;
  if (len == 0) return true;
  if (!buffer_) {
    if (!ensureFile()) return false;
    const int n = file_.write(data, len);
    if (n != static_cast<int>(len)) return false;
    written_ += len;
    return true;
  }
  while (len > 0) {
    const size_t take = std::min(len, kBufferBytes - used_);
    std::memcpy(buffer_ + used_, data, take);
    used_ += take;
    data += take;
    len -= take;
    written_ += take;
    if (used_ == kBufferBytes && !flushBuffer()) return false;
  }
  return true;
}

bool PartFileSink::flush() {
  if (!open_ || !flushBuffer()) return false;
  if (fileReady_) file_.flush();
  return true;
}

void PartFileSink::close() {
  if (open_) {
    (void)flush();
    if (fileReady_) file_.close();
    open_ = false;
    fileReady_ = false;
  }
  if (buffer_) {
#if defined(ARDUINO_ARCH_ESP32)
    heap_caps_free(buffer_);
#else
    std::free(buffer_);
#endif
    buffer_ = nullptr;
  }
  used_ = 0;
  written_ = 0;
}

bool loadCookieHeader(const std::string& appDataRoot, const std::string& providerId,
                      std::string& cookieOut) {
  cookieOut.clear();
  std::string raw;
  if (!readSmall(configPath(appDataRoot), raw)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, raw)) return false;
  JsonObject cookies = doc["cookies"].as<JsonObject>();
  if (cookies.isNull()) return false;

  auto add = [&](const std::string& k, const std::string& v) {
    if (k.empty() || v.empty()) return;
    if (!cookieOut.empty()) cookieOut += "; ";
    cookieOut += k;
    cookieOut += '=';
    cookieOut += v;
  };

  if (providerId == "weread") {
    const std::string vid = cookies["wr_vid"] | "";
    const std::string skey = cookies["wr_skey"] | "";
    if (vid.empty() || skey.empty()) return false;

    bool hasLocalVid = false;
    for (JsonPair kv : cookies) {
      const std::string key = kv.key().c_str();
      if (!M4xNetPolicy::isWereadCookieName(key)) continue;
      std::string value;
      if (kv.value().is<const char*>()) value = kv.value().as<const char*>();
      else if (kv.value().is<long>()) value = std::to_string(kv.value().as<long>());
      else if (kv.value().is<unsigned long>()) value = std::to_string(kv.value().as<unsigned long>());
      else continue;
      if (value.empty()) continue;
      add(key, value);
      if (M4xNetPolicy::toLowerAscii(key) == "wr_localvid") hasLocalVid = true;
    }
    if (!hasLocalVid) add("wr_localvid", vid);
    return true;
  }

  // JJWXC may add/change cookie names server-side; preserve all scalar values.
  for (JsonPair kv : cookies) {
    if (!kv.value().is<const char*>() && !kv.value().is<long>() && !kv.value().is<unsigned long>()) continue;
    std::string v;
    if (kv.value().is<const char*>()) v = kv.value().as<const char*>();
    else if (kv.value().is<long>()) v = std::to_string(kv.value().as<long>());
    else v = std::to_string(kv.value().as<unsigned long>());
    add(kv.key().c_str(), v);
  }
  return !cookieOut.empty();
}

bool hasCredential(const std::string& appDataRoot, const std::string& providerId) {
  std::string ignored;
  return loadCookieHeader(appDataRoot, providerId, ignored);
}

bool mergeSetCookies(const std::string& appDataRoot, const std::string& providerId,
                     const std::vector<std::string>& lines) {
  if (lines.empty()) return false;
  const std::string path = configPath(appDataRoot);
  std::string raw;
  JsonDocument doc;
  if (readSmall(path, raw)) (void)deserializeJson(doc, raw);
  JsonObject cookies;
  if (doc["cookies"].is<JsonObject>()) cookies = doc["cookies"].as<JsonObject>();
  else cookies = doc["cookies"].to<JsonObject>();

  bool changed = false;
  for (const auto& line : lines) {
    const size_t semi = line.find(';');
    const std::string nv = line.substr(0, semi);
    const size_t eq = nv.find('=');
    if (eq == std::string::npos || eq == 0) continue;
    std::string name = trim(nv.substr(0, eq));
    const std::string value = nv.substr(eq + 1);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    if (name.empty()) continue;
    if (providerId == "weread" && !M4xNetPolicy::isWereadCookieName(name)) continue;
    cookies[name] = value;
    changed = true;
  }
  if (!changed) return false;

  std::string out;
  serializeJson(doc, out);
  if (out.size() > 16u * 1024u) return false;
  const std::string tmp = path + ".tmp";
  if (!writeExact(tmp, out)) return false;
  if (SdMan.exists(path.c_str())) SdMan.remove(path.c_str());
  if (!SdMan.rename(tmp.c_str(), path.c_str())) {
    SdMan.remove(tmp.c_str());
    return false;
  }
  return true;
}

}  // namespace M4NativeProviderIo
