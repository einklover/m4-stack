#include "HttpDownloader.h"

#include <HTTPClient.h>
#include <HardwareSerial.h>
#include <StreamString.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <base64.h>

#include <cstring>
#include <memory>

#include "CrossPointSettings.h"
#include "M4HttpDownloadPolicy.h"
#include "util/UrlUtils.h"

namespace {
class BoundedWriteStream final : public Stream {
 public:
  BoundedWriteStream(Stream& target, WiFiClient& client, size_t limit)
      : target_(target), client_(client), limit_(limit), started_(millis()), progressed_(started_) {}
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* data, size_t size) override {
    const uint32_t now = millis();
    if (M4HttpDownloadPolicy::timedOut(now, started_, progressed_) ||
        now - started_ >= M4HttpDownloadPolicy::kTextTotalTimeoutMs ||
        M4HttpDownloadPolicy::exceeds(written_, size, limit_)) {
      failed_ = true;
      client_.stop();
      return 0;
    }
    if (size == 0) return 0;
    const size_t n = target_.write(data, size);
    written_ += n;
    if (n != size) failed_ = true;
    if (n) progressed_ = millis();
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
};
}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent) {
  return fetchUrlBounded(url, outContent, M4HttpDownloadPolicy::kMaxFeedBytes);
}

bool HttpDownloader::fetchUrlBounded(const std::string& url, Stream& outContent, size_t maxBytes) {
  // Use WiFiClientSecure for HTTPS, regular WiFiClient for HTTP
  std::unique_ptr<WiFiClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new WiFiClientSecure();
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    client.reset(new WiFiClient());
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
  BoundedWriteStream bounded(outContent, *source, maxBytes);
  const int copied = http.writeToStream(&bounded);
  http.end();
  if (copied < 0 || bounded.failed() || static_cast<size_t>(copied) != bounded.written() ||
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
  // Use WiFiClientSecure for HTTPS, regular WiFiClient for HTTP
  std::unique_ptr<WiFiClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new WiFiClientSecure();
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    client.reset(new WiFiClient());
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

  const size_t rawSize = http.getSize();
  // ESP32 HTTPClient returns -1 (0xFFFFFFFF as size_t) when Content-Length is unknown
  const size_t contentLength = (rawSize == (size_t)-1) ? 0 : rawSize;
  const size_t effectiveMax = maxBytes < M4HttpDownloadPolicy::kMaxFileBytes
                                  ? maxBytes : M4HttpDownloadPolicy::kMaxFileBytes;
  Serial.printf("[%lu] [HTTP] Content-Length: %zu\n", millis(), contentLength);
  if (contentLength > effectiveMax) {
    http.end();
    return HTTP_ERROR;
  }

  // Remove existing file if present
  if (SdMan.exists(destPath.c_str())) {
    SdMan.remove(destPath.c_str());
  }

  // Open file for writing
  FsFile file;
  if (!SdMan.openFileForWrite("HTTP", destPath.c_str(), file)) {
    Serial.printf("[%lu] [HTTP] Failed to open file for writing\n", millis());
    http.end();
    return FILE_ERROR;
  }

  // Get the stream for chunked reading
  WiFiClient* stream = http.getStreamPtr();
  if (!stream) {
    Serial.printf("[%lu] [HTTP] Failed to get stream\n", millis());
    file.close();
    SdMan.remove(destPath.c_str());
    http.end();
    return HTTP_ERROR;
  }

  // Download in chunks
  uint8_t buffer[DOWNLOAD_CHUNK_SIZE];
  size_t downloaded = 0;
  const size_t total = contentLength > 0 ? contentLength : 0;
  const uint32_t started = millis();
  uint32_t progressed = started;

  while (http.connected() && (contentLength == 0 || downloaded < contentLength)) {
    if (M4HttpDownloadPolicy::timedOut(millis(), started, progressed)) {
      file.close();
      SdMan.remove(destPath.c_str());
      http.end();
      return HTTP_ERROR;
    }
    const size_t available = stream->available();
    if (available == 0) {
      delay(1);
      continue;
    }

    const size_t toRead = available < DOWNLOAD_CHUNK_SIZE ? available : DOWNLOAD_CHUNK_SIZE;
    if (M4HttpDownloadPolicy::exceeds(downloaded, toRead, effectiveMax)) {
      file.close();
      SdMan.remove(destPath.c_str());
      http.end();
      return HTTP_ERROR;
    }
    const size_t bytesRead = stream->readBytes(buffer, toRead);

    if (bytesRead == 0) {
      break;
    }

    const size_t written = file.write(buffer, bytesRead);
    if (written != bytesRead) {
      Serial.printf("[%lu] [HTTP] Write failed: wrote %zu of %zu bytes\n", millis(), written, bytesRead);
      file.close();
      SdMan.remove(destPath.c_str());
      http.end();
      return FILE_ERROR;
    }

    downloaded += bytesRead;
    progressed = millis();

    if (progress && total > 0) {
      progress(downloaded, total);
    }
  }

  file.close();
  http.end();

  Serial.printf("[%lu] [HTTP] Downloaded %zu bytes\n", millis(), downloaded);

  // Verify download size if known
  if (contentLength > 0 && downloaded != contentLength) {
    Serial.printf("[%lu] [HTTP] Size mismatch: got %zu, expected %zu\n", millis(), downloaded, contentLength);
    SdMan.remove(destPath.c_str());
    return HTTP_ERROR;
  }

  return OK;
}
HttpDownloader::DownloadError HttpDownloader::downloadToFile_jg(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress) {
  // Use WiFiClientSecure for HTTPS, regular WiFiClient for HTTP
  std::unique_ptr<WiFiClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new WiFiClientSecure();
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    client.reset(new WiFiClient());
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

  const size_t rawSize_jg = http.getSize();
  const size_t contentLength = (rawSize_jg == (size_t)-1) ? 0 : rawSize_jg;
  if (contentLength > M4HttpDownloadPolicy::kMaxFileBytes) {
    http.end();
    return HTTP_ERROR;
  }
  Serial.printf("[%lu] [HTTP] Content-Length: %zu\n", millis(), contentLength);

  // Remove existing file if present
  if (SdMan.exists(destPath.c_str())) {
    SdMan.remove(destPath.c_str());
  }

  // Open file for writing
  FsFile file;
  if (!SdMan.openFileForWrite("HTTP", destPath.c_str(), file)) {
    Serial.printf("[%lu] [HTTP] Failed to open file for writing\n", millis());
    http.end();
    return FILE_ERROR;
  }

  // Get the stream for chunked reading
  WiFiClient* stream = http.getStreamPtr();
  if (!stream) {
    Serial.printf("[%lu] [HTTP] Failed to get stream\n", millis());
    file.close();
    SdMan.remove(destPath.c_str());
    http.end();
    return HTTP_ERROR;
  }

  // Download in chunks
  uint8_t buffer[DOWNLOAD_CHUNK_SIZE];
  size_t downloaded = 0;
  const size_t total = contentLength > 0 ? contentLength : 0;
  const uint32_t started = millis();
  uint32_t progressed = started;

  while (http.connected() && (contentLength == 0 || downloaded < contentLength)) {
    if (M4HttpDownloadPolicy::timedOut(millis(), started, progressed)) {
      file.close();
      SdMan.remove(destPath.c_str());
      http.end();
      return HTTP_ERROR;
    }
    const size_t available = stream->available();
    if (available == 0) {
      delay(1);
      continue;
    }

    const size_t toRead = available < DOWNLOAD_CHUNK_SIZE ? available : DOWNLOAD_CHUNK_SIZE;
    if (M4HttpDownloadPolicy::exceeds(downloaded, toRead, M4HttpDownloadPolicy::kMaxFileBytes)) {
      file.close();
      SdMan.remove(destPath.c_str());
      http.end();
      return HTTP_ERROR;
    }
    const size_t bytesRead = stream->readBytes(buffer, toRead);

    if (bytesRead == 0) {
      break;
    }

    const size_t written = file.write(buffer, bytesRead);
    if (written != bytesRead) {
      Serial.printf("[%lu] [HTTP] Write failed: wrote %zu of %zu bytes\n", millis(), written, bytesRead);
      file.close();
      SdMan.remove(destPath.c_str());
      http.end();
      return FILE_ERROR;
    }

    downloaded += bytesRead;
    progressed = millis();

    if (progress && total > 0) {
      progress(downloaded, total);
    }
  }

  file.close();
  http.end();

  Serial.printf("[%lu] [HTTP] Downloaded %zu bytes\n", millis(), downloaded);

  // Verify download size if known
  if (contentLength > 0 && downloaded != contentLength) {
    Serial.printf("[%lu] [HTTP] Size mismatch: got %zu, expected %zu\n", millis(), downloaded, contentLength);
    SdMan.remove(destPath.c_str());
    return HTTP_ERROR;
  }

  return OK;
}

// 数据胶囊专用下载函数（使用 Zotero User-Agent）
HttpDownloader::DownloadError HttpDownloader::downloadToFile_dc(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress) {
  std::unique_ptr<WiFiClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new WiFiClientSecure();
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    client.reset(new WiFiClient());
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

  const size_t rawSize_dc = http.getSize();
  const size_t contentLength = (rawSize_dc == (size_t)-1) ? 0 : rawSize_dc;
  if (contentLength > M4HttpDownloadPolicy::kMaxFileBytes) {
    http.end();
    return HTTP_ERROR;
  }
  Serial.printf("[%lu] [DC] Content-Length: %zu\n", millis(), contentLength);

  if (SdMan.exists(destPath.c_str())) {
    SdMan.remove(destPath.c_str());
  }

  FsFile file;
  if (!SdMan.openFileForWrite("DC", destPath.c_str(), file)) {
    Serial.printf("[%lu] [DC] Failed to open file for writing\n", millis());
    http.end();
    return FILE_ERROR;
  }

  WiFiClient* stream = http.getStreamPtr();
  if (!stream) {
    Serial.printf("[%lu] [DC] Failed to get stream\n", millis());
    file.close();
    SdMan.remove(destPath.c_str());
    http.end();
    return HTTP_ERROR;
  }

  uint8_t buffer[DOWNLOAD_CHUNK_SIZE];
  size_t downloaded = 0;
  const size_t total = contentLength > 0 ? contentLength : 0;
  const uint32_t started = millis();
  uint32_t progressed = started;

  while (http.connected() && (contentLength == 0 || downloaded < contentLength)) {
    if (M4HttpDownloadPolicy::timedOut(millis(), started, progressed)) {
      file.close();
      SdMan.remove(destPath.c_str());
      http.end();
      return HTTP_ERROR;
    }
    const size_t available = stream->available();
    if (available == 0) {
      delay(1);
      continue;
    }

    const size_t toRead = available < DOWNLOAD_CHUNK_SIZE ? available : DOWNLOAD_CHUNK_SIZE;
    if (M4HttpDownloadPolicy::exceeds(downloaded, toRead, M4HttpDownloadPolicy::kMaxFileBytes)) {
      file.close();
      SdMan.remove(destPath.c_str());
      http.end();
      return HTTP_ERROR;
    }
    const size_t bytesRead = stream->readBytes(buffer, toRead);

    if (bytesRead == 0) {
      break;
    }

    const size_t written = file.write(buffer, bytesRead);
    if (written != bytesRead) {
      Serial.printf("[%lu] [DC] Write failed: wrote %zu of %zu bytes\n", millis(), written, bytesRead);
      file.close();
      SdMan.remove(destPath.c_str());
      http.end();
      return FILE_ERROR;
    }

    downloaded += bytesRead;
    progressed = millis();

    if (progress && total > 0) {
      progress(downloaded, total);
    }
  }

  file.close();
  http.end();

  Serial.printf("[%lu] [DC] Downloaded %zu bytes\n", millis(), downloaded);

  if (contentLength > 0 && downloaded != contentLength) {
    Serial.printf("[%lu] [DC] Size mismatch: got %zu, expected %zu\n", millis(), downloaded, contentLength);
    SdMan.remove(destPath.c_str());
    return HTTP_ERROR;
  }

  return OK;
}
