#include "HttpDownloader.h"

#include <HTTPClient.h>
#include <HardwareSerial.h>
#include <StreamString.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <base64.h>

#include <cstring>
#include <algorithm>
#include <memory>

#include "CrossPointSettings.h"
#include "M4HttpDownloadPolicy.h"
#include "M4DeadlineClient.h"
#include "util/UrlUtils.h"
#include "util/M4DownloadCommit.h"
#include "apps/providers/M4NativeProviderHeavyGate.h"

namespace {
void startBody(WiFiClient& client, bool secure, uint32_t totalMs) {
  if (secure) static_cast<M4DeadlineClient<WiFiClientSecure>&>(client).startBody(totalMs);
  else static_cast<M4DeadlineClient<WiFiClient>&>(client).startBody(totalMs);
}
bool bodyExpired(WiFiClient& client, bool secure) {
  return secure ? static_cast<M4DeadlineClient<WiFiClientSecure>&>(client).expired()
                : static_cast<M4DeadlineClient<WiFiClient>&>(client).expired();
}
class BoundedWriteStream final : public Stream {
 public:
  BoundedWriteStream(Stream& target, WiFiClient& client, size_t limit,
                     uint32_t totalMs = M4HttpDownloadPolicy::kTextTotalTimeoutMs,
                     HttpDownloader::ProgressCallback progress = nullptr, size_t total = 0)
      : target_(target), client_(client), limit_(limit), started_(millis()), progressed_(started_),
        totalMs_(totalMs), progress_(progress), total_(total) {}
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* data, size_t size) override {
    const uint32_t now = millis();
    if (M4HttpDownloadPolicy::timedOut(now, started_, progressed_) ||
        now - started_ >= totalMs_ ||
        M4HttpDownloadPolicy::exceeds(written_, size, limit_)) {
      failed_ = true;
      client_.stop();
      return 0;
    }
    if (size == 0) return 0;
    const size_t n = target_.write(data, size);
    written_ += n;
    if (n != size) { failed_ = true; client_.stop(); }
    if (n) progressed_ = millis();
    if (progress_ && (written_ - lastNotified_ >= 8192 || now - lastNotifyMs_ >= 250)) {
      lastNotified_ = written_; lastNotifyMs_ = now;
      progress_(written_, total_);
    }
    delay(1);
    return n;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  bool failed() const { return failed_; }
  size_t written() const { return written_; }
 private:
  Stream& target_;
  WiFiClient& client_;
  size_t limit_;
  size_t written_ = 0;
  uint32_t started_;
  uint32_t progressed_;
  bool failed_ = false;
  uint32_t totalMs_;
  HttpDownloader::ProgressCallback progress_;
  size_t total_, lastNotified_ = 0;
  uint32_t lastNotifyMs_ = 0;
};
HttpDownloader::DownloadError downloadBody(HTTPClient& http, WiFiClient& client,
    const std::string& url, const std::string& destPath, size_t limit,
    HttpDownloader::ProgressCallback progress) {
  const int length = http.getSize();
  if (length >= 0 && static_cast<size_t>(length) > limit) { http.end(); return HttpDownloader::HTTP_ERROR; }
  const std::string tmp = destPath + ".m4-download.tmp";
  const std::string backup = destPath + ".m4-download.bak";
  if (SdMan.exists(backup.c_str())) { http.end(); return HttpDownloader::FILE_ERROR; }
  FsFile file = SdMan.open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL);
  if (!file) { http.end(); return HttpDownloader::FILE_ERROR; }
  startBody(client, UrlUtils::isHttpsUrl(url), M4HttpDownloadPolicy::kTotalTimeoutMs);
  BoundedWriteStream sink(file, client, limit, M4HttpDownloadPolicy::kTotalTimeoutMs,
                          progress, length > 0 ? static_cast<size_t>(length) : 0);
  // HTTPClient owns chunk framing. Copying getStreamPtr() directly corrupts
  // chunked books with hexadecimal sizes and CRLFs.
  const int copied = http.writeToStream(&sink);
  const bool complete = !bodyExpired(client, UrlUtils::isHttpsUrl(url)) && !sink.failed() &&
                        copied >= 0 && static_cast<size_t>(copied) == sink.written() &&
                        (length < 0 || sink.written() == static_cast<size_t>(length));
  const bool synced = complete && !file.getWriteError() && file.sync();
  const bool closed = file.close();
  http.end();
  if (!complete || !synced || !closed) {
    SdMan.remove(tmp.c_str());
    return complete ? HttpDownloader::FILE_ERROR : HttpDownloader::HTTP_ERROR;
  }
  if (!M4DownloadCommit::commit(SdMan, tmp.c_str(), destPath.c_str(), backup.c_str())) {
    // Keep both copies if a rename/rollback failed. A later request refuses
    // these reserved sibling names instead of truncating recovery data.
    Serial.printf("[HTTP] Download commit failed; recovery copies retained\n");
    return HttpDownloader::FILE_ERROR;
  }
  if (progress) progress(sink.written(), sink.written());
  return HttpDownloader::OK;
}
}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent) {
  return fetchUrlBounded(url, outContent, M4HttpDownloadPolicy::kMaxFeedBytes);
}

bool HttpDownloader::fetchUrlBounded(const std::string& url, Stream& outContent, size_t maxBytes) {
  M4NativeProviderHeavyGate::Lock networkLock(M4NativeProviderHeavyGate::mutex());
  // Use WiFiClientSecure for HTTPS, regular WiFiClient for HTTP
  std::unique_ptr<WiFiClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new M4DeadlineClient<WiFiClientSecure>();
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    client.reset(new M4DeadlineClient<WiFiClient>());
  }
  HTTPClient http;

  Serial.printf("[%lu] [HTTP] Fetching: %s\n", millis(), url.c_str());

  http.begin(*client, url.c_str());
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(M4HttpDownloadPolicy::kIdleTimeoutMs);
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);

  // Add Basic HTTP auth if credentials are configured
  if (strlen(SETTINGS.opdsUsername) > 0 && strlen(SETTINGS.opdsPassword) > 0) {
    std::string credentials = std::string(SETTINGS.opdsUsername) + ":" + SETTINGS.opdsPassword;
    String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", "Basic " + encoded);
  }

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[%lu] [HTTP] Fetch failed: %d\n", millis(), httpCode);
    http.end();
    return false;
  }

  const int length = http.getSize();
  if (length >= 0 && static_cast<size_t>(length) > maxBytes) {
    http.end();
    return false;
  }
  WiFiClient* source = http.getStreamPtr();
  if (!source) {
    http.end();
    return false;
  }
  // HTTPClient decodes chunked transfer framing here. The wrapper caps the
  // decoded body and stops a stalled or overly long identity stream.
  startBody(*client, UrlUtils::isHttpsUrl(url), M4HttpDownloadPolicy::kTextTotalTimeoutMs);
  BoundedWriteStream bounded(outContent, *source, maxBytes);
  const int copied = http.writeToStream(&bounded);
  http.end();
  if (bodyExpired(*client, UrlUtils::isHttpsUrl(url)) || copied < 0 || bounded.failed() || static_cast<size_t>(copied) != bounded.written() ||
      (length >= 0 && bounded.written() != static_cast<size_t>(length))) return false;
  Serial.printf("[%lu] [HTTP] Fetch success bytes=%zu\n", millis(), bounded.written());
  return true;
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent) {
  StreamString stream;
  if (!fetchUrlBounded(url, stream, M4HttpDownloadPolicy::kMaxTextBytes)) {
    return false;
  }
  outContent = stream.c_str();
  return true;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress) {
  return downloadToFileBounded(url, destPath, static_cast<size_t>(-1), progress);
}

HttpDownloader::DownloadError HttpDownloader::downloadToFileBounded(const std::string& url,
                                                                     const std::string& destPath,
                                                                     const size_t maxBytes,
                                                                     ProgressCallback progress) {
  M4NativeProviderHeavyGate::Lock networkLock(M4NativeProviderHeavyGate::mutex());
  // Use WiFiClientSecure for HTTPS, regular WiFiClient for HTTP
  std::unique_ptr<WiFiClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new M4DeadlineClient<WiFiClientSecure>();
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    client.reset(new M4DeadlineClient<WiFiClient>());
  }
  HTTPClient http;

  Serial.printf("[%lu] [HTTP] Downloading: %s\n", millis(), url.c_str());
  Serial.printf("[%lu] [HTTP] Destination: %s\n", millis(), destPath.c_str());

  http.begin(*client, url.c_str());
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(M4HttpDownloadPolicy::kIdleTimeoutMs);
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);

  // Add Basic HTTP auth if credentials are configured
  if (strlen(SETTINGS.opdsUsername) > 0 && strlen(SETTINGS.opdsPassword) > 0) {
    std::string credentials = std::string(SETTINGS.opdsUsername) + ":" + SETTINGS.opdsPassword;
    String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", "Basic " + encoded);
  }

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[%lu] [HTTP] Download failed: %d\n", millis(), httpCode);
    http.end();
    return HTTP_ERROR;
  }

  return downloadBody(http, *client, url, destPath, std::min(maxBytes, M4HttpDownloadPolicy::kMaxFileBytes), progress);
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile_jg(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress) {
  M4NativeProviderHeavyGate::Lock networkLock(M4NativeProviderHeavyGate::mutex());
  // Use WiFiClientSecure for HTTPS, regular WiFiClient for HTTP
  std::unique_ptr<WiFiClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new M4DeadlineClient<WiFiClientSecure>();
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    client.reset(new M4DeadlineClient<WiFiClient>());
  }
  HTTPClient http;

  Serial.printf("[%lu] [HTTP] Downloading: %s\n", millis(), url.c_str());
  Serial.printf("[%lu] [HTTP] Destination: %s\n", millis(), destPath.c_str());

  http.begin(*client, url.c_str());
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(M4HttpDownloadPolicy::kIdleTimeoutMs);
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);

  // Add Basic HTTP auth if credentials are configured
  if (strlen(SETTINGS.jgUsername) > 0 && strlen(SETTINGS.jgAppPassword) > 0) {
    std::string credentials = std::string(SETTINGS.jgUsername) + ":" + SETTINGS.jgAppPassword;
    String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", "Basic " + encoded);
  }

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[%lu] [HTTP] Download failed: %d\n", millis(), httpCode);
    http.end();
    return HTTP_ERROR;
  }

  return downloadBody(http, *client, url, destPath, M4HttpDownloadPolicy::kMaxFileBytes, progress);
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile_dc(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress) {
  M4NativeProviderHeavyGate::Lock networkLock(M4NativeProviderHeavyGate::mutex());
  std::unique_ptr<WiFiClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new M4DeadlineClient<WiFiClientSecure>();
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    client.reset(new M4DeadlineClient<WiFiClient>());
  }
  HTTPClient http;

  Serial.printf("[%lu] [DC] Downloading: %s\n", millis(), url.c_str());
  Serial.printf("[%lu] [DC] Destination: %s\n", millis(), destPath.c_str());

  http.begin(*client, url.c_str());
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(M4HttpDownloadPolicy::kIdleTimeoutMs);
  // 关键：使用 Zotero User-Agent（必须用 setUserAgent，addHeader 会被覆盖）
  http.setUserAgent("Zotero/7.0");

  // 添加数据胶囊的认证信息
  if (strlen(SETTINGS.dcUsername) > 0 && strlen(SETTINGS.dcPassword) > 0) {
    std::string credentials = std::string(SETTINGS.dcUsername) + ":" + SETTINGS.dcPassword;
    String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", "Basic " + encoded);
  }

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[%lu] [DC] Download failed: %d\n", millis(), httpCode);
    http.end();
    return HTTP_ERROR;
  }

  return downloadBody(http, *client, url, destPath, M4HttpDownloadPolicy::kMaxFileBytes, progress);
}
