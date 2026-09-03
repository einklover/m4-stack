#include "network/M4FileTransferHttpRoutes.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <SDCardManager.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "CrossPointSettings.h"
#include "SettingsLists.h"
#include "apps/providers/M4LanVisitorStore.h"
#include "network/M4HttpRequestParser.h"
#include "network/html/FilesPageHtml.generated.h"
#include "network/html/HomePageHtml.generated.h"
#include "network/html/SettingsPageHtml.generated.h"
#include "qemu/M4QemuNet.h"
#include "util/StringUtils.h"

namespace {
constexpr const char* HIDDEN_ITEMS[] = {"System Volume Information", "XTCache"};
constexpr size_t HIDDEN_ITEMS_COUNT = sizeof(HIDDEN_ITEMS) / sizeof(HIDDEN_ITEMS[0]);
constexpr size_t HTTP_QUERY_MAX_SIZE = 2048;
constexpr size_t HTTP_HEADER_MAX_SIZE = 1024;

struct FileInfo {
  String name;
  size_t size = 0;
  bool isEpub = false;
  bool isDirectory = false;
};

class StorageGuard {
 public:
  explicit StorageGuard(const SemaphoreHandle_t mutex) : mutex_(mutex) {
    locked_ = mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE;
  }
  ~StorageGuard() {
    if (locked_) xSemaphoreGive(mutex_);
  }
  explicit operator bool() const { return locked_; }

 private:
  SemaphoreHandle_t mutex_ = nullptr;
  bool locked_ = false;
};

const char* statusLine(const int status) {
  switch (status) {
    case 200:
      return "200 OK";
    case 400:
      return "400 Bad Request";
    case 403:
      return "403 Forbidden";
    case 404:
      return "404 Not Found";
    case 409:
      return "409 Conflict";
    case 413:
      return "413 Payload Too Large";
    case 500:
      return "500 Internal Server Error";
    case 503:
      return "503 Service Unavailable";
    default:
      return "500 Internal Server Error";
  }
}

esp_err_t sendResponse(httpd_req_t* req, const int status, const char* contentType, const char* body) {
  httpd_resp_set_status(req, statusLine(status));
  httpd_resp_set_type(req, contentType ? contentType : "text/plain");
  return httpd_resp_send(req, body ? body : "", body ? HTTPD_RESP_USE_STRLEN : 0);
}

esp_err_t sendResponse(httpd_req_t* req, const int status, const char* contentType, const String& body) {
  httpd_resp_set_status(req, statusLine(status));
  httpd_resp_set_type(req, contentType ? contentType : "text/plain");
  return httpd_resp_send(req, body.c_str(), body.length());
}

bool readHeader(httpd_req_t* req, const char* name, std::string& value) {
  value.clear();
  const size_t len = httpd_req_get_hdr_value_len(req, name);
  if (len == 0 || len > HTTP_HEADER_MAX_SIZE) return false;
  std::vector<char> buffer(len + 1, '\0');
  if (httpd_req_get_hdr_value_str(req, name, buffer.data(), buffer.size()) != ESP_OK) return false;
  value.assign(buffer.data(), len);
  return true;
}

bool queryArg(httpd_req_t* req, const char* key, String& value) {
  value = "";
  const size_t len = httpd_req_get_url_query_len(req);
  if (len == 0 || len > HTTP_QUERY_MAX_SIZE) return false;

  std::vector<char> query(len + 1, '\0');
  if (httpd_req_get_url_query_str(req, query.data(), query.size()) != ESP_OK) return false;

  std::array<char, HTTP_QUERY_MAX_SIZE + 1> raw{};
  if (httpd_query_key_value(query.data(), key, raw.data(), raw.size()) != ESP_OK) return false;
  value = M4HttpRequestParser::urlDecode(raw.data()).c_str();
  return true;
}

bool recvBody(httpd_req_t* req, std::string& body, const size_t maxBody) {
  body.clear();
  if (req->content_len < 0) return false;
  const size_t contentLength = static_cast<size_t>(req->content_len);
  if (contentLength > maxBody) return false;
  body.reserve(contentLength);

  std::array<char, M4FileTransferHttpRoutes::HTTP_BODY_CHUNK_SIZE> chunk{};
  size_t remaining = contentLength;
  unsigned timeoutRetries = 0;
  while (remaining > 0) {
    const size_t wanted = std::min(remaining, chunk.size());
    const int received = httpd_req_recv(req, chunk.data(), wanted);
    if (received == HTTPD_SOCK_ERR_TIMEOUT && timeoutRetries++ < 3) {
      esp_task_wdt_reset();
      continue;
    }
    if (received <= 0) return false;
    timeoutRetries = 0;
    body.append(chunk.data(), static_cast<size_t>(received));
    remaining -= static_cast<size_t>(received);
    esp_task_wdt_reset();
  }
  return true;
}

bool readFormFields(httpd_req_t* req, std::vector<M4HttpRequestParser::Field>& fields) {
  std::string contentType;
  if (!readHeader(req, "Content-Type", contentType)) return false;

  std::string body;
  if (!recvBody(req, body, M4FileTransferHttpRoutes::HTTP_CONTROL_BODY_SIZE)) return false;

  std::string boundary;
  if (M4HttpRequestParser::extractMultipartBoundary(contentType, boundary)) {
    return M4HttpRequestParser::parseMultipartFields(body, boundary, fields);
  }
  if (contentType.find("application/x-www-form-urlencoded") != std::string::npos) {
    return M4HttpRequestParser::parseUrlEncodedFields(body, fields);
  }
  return false;
}

String formField(const std::vector<M4HttpRequestParser::Field>& fields, const char* name) {
  return M4HttpRequestParser::fieldValue(fields, name).c_str();
}

String normalizeWebPath(const String& inputPath) {
  if (inputPath.isEmpty() || inputPath == "/") return "/";
  std::string normalized = FsHelpers::normalisePath(inputPath.c_str());
  String result = normalized.c_str();
  if (result.isEmpty()) return "/";
  if (!result.startsWith("/")) result = "/" + result;
  if (result.length() > 1 && result.endsWith("/")) result = result.substring(0, result.length() - 1);
  return result;
}

bool isProtectedItemName(const String& name) {
  if (name.startsWith(".")) return true;
  for (size_t i = 0; i < HIDDEN_ITEMS_COUNT; ++i) {
    if (name.equals(HIDDEN_ITEMS[i])) return true;
  }
  return false;
}

bool isEpubFile(const String& filename) {
  String lower = filename;
  lower.toLowerCase();
  return lower.endsWith(".epub");
}

void clearEpubCacheIfNeeded(const String& filePath) {
  if (!StringUtils::checkFileExtension(filePath, ".epub")) return;
  Epub(filePath.c_str(), "/.crosspoint").clearCache();
  Serial.printf("[%lu] [HTTPD] Cleared epub cache for: %s\n", millis(), filePath.c_str());
}

void noteTransferVisitor(httpd_req_t* req) {
  if (!req || !M4QemuNet::staConnected()) return;
  const int socketFd = httpd_req_to_sockfd(req);
  if (socketFd < 0) return;

  sockaddr_storage peer{};
  socklen_t peerLen = sizeof(peer);
  if (getpeername(socketFd, reinterpret_cast<sockaddr*>(&peer), &peerLen) != 0) return;
  if (peer.ss_family != AF_INET) return;

  char ipBuf[INET_ADDRSTRLEN] = {0};
  const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&peer);
  if (!inet_ntop(AF_INET, &ipv4->sin_addr, ipBuf, sizeof(ipBuf))) return;

  const String ssid = M4QemuNet::ssidString();
  if (ssid.isEmpty()) return;
  M4LanVisitorStore::note(ssid.c_str(), ipBuf);
}

template <typename Callback>
void scanFiles(const char* path, Callback callback) {
  FsFile root = SdMan.open(path);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  FsFile file = root.openNextFile();
  char name[500];
  while (file) {
    file.getName(name, sizeof(name));
    const String fileName = name;
    if (!isProtectedItemName(fileName)) {
      FileInfo info;
      info.name = fileName;
      info.isDirectory = file.isDirectory();
      info.size = info.isDirectory ? 0 : file.size();
      info.isEpub = !info.isDirectory && isEpubFile(info.name);
      callback(info);
    }
    file.close();
    yield();
    esp_task_wdt_reset();
    file = root.openNextFile();
  }
  root.close();
}

bool writeFileBytes(FsFile& file, const char* data, const size_t length) {
  if (length == 0) return true;
  esp_task_wdt_reset();
  const size_t written = file.write(reinterpret_cast<const uint8_t*>(data), length);
  esp_task_wdt_reset();
  return written == length;
}

}  // namespace

esp_err_t M4FileTransferHttpRoutes::handleRoot(httpd_req_t* req) const {
  noteTransferVisitor(req);
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, HomePageHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t M4FileTransferHttpRoutes::handleFileList(httpd_req_t* req) const {
  noteTransferVisitor(req);
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, FilesPageHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t M4FileTransferHttpRoutes::handleStatus(httpd_req_t* req) const {
  noteTransferVisitor(req);
  const wifi_mode_t wifiMode = WiFi.getMode();
  const bool apMode = (wifiMode & WIFI_MODE_AP) && !m4QemuNetWifiCompatConnected();
  const String ipAddr = apMode ? WiFi.softAPIP().toString() : M4QemuNet::localIpString();

  JsonDocument doc;
  doc["version"] = CROSSPOINT_VERSION;
  doc["ip"] = ipAddr;
  doc["mode"] = apMode ? "AP" : "STA";
  doc["ssid"] = apMode ? String() : M4QemuNet::ssidString();
  doc["rssi"] = apMode ? 0 : M4QemuNet::rssiOrZero();
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["uptime"] = millis() / 1000;

  String json;
  serializeJson(doc, json);
  return sendResponse(req, 200, "application/json", json);
}

esp_err_t M4FileTransferHttpRoutes::handleFileListData(httpd_req_t* req) const {
  String currentPath = "/";
  String requestedPath;
  if (queryArg(req, "path", requestedPath)) currentPath = normalizeWebPath(requestedPath);

  StorageGuard lock(storageMutex_);
  if (!lock) return sendResponse(req, 503, "text/plain", "Storage unavailable");

  httpd_resp_set_type(req, "application/json");
  if (httpd_resp_send_chunk(req, "[", 1) != ESP_OK) return ESP_FAIL;

  bool seenFirst = false;
  esp_err_t streamResult = ESP_OK;
  char output[512];
  JsonDocument doc;
  scanFiles(currentPath.c_str(), [&](const FileInfo& info) {
    if (streamResult != ESP_OK) return;
    doc.clear();
    doc["name"] = info.name;
    doc["size"] = info.size;
    doc["isDirectory"] = info.isDirectory;
    doc["isEpub"] = info.isEpub;
    const size_t written = serializeJson(doc, output, sizeof(output));
    if (written >= sizeof(output)) return;
    if (seenFirst && httpd_resp_send_chunk(req, ",", 1) != ESP_OK) {
      streamResult = ESP_FAIL;
      return;
    }
    seenFirst = true;
    if (httpd_resp_send_chunk(req, output, written) != ESP_OK) streamResult = ESP_FAIL;
  });

  if (streamResult != ESP_OK) return streamResult;
  if (httpd_resp_send_chunk(req, "]", 1) != ESP_OK) return ESP_FAIL;
  return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t M4FileTransferHttpRoutes::handleDownload(httpd_req_t* req) const {
  String itemPath;
  if (!queryArg(req, "path", itemPath)) return sendResponse(req, 400, "text/plain", "Missing path");
  if (itemPath.isEmpty() || itemPath == "/") return sendResponse(req, 400, "text/plain", "Invalid path");
  if (!itemPath.startsWith("/")) itemPath = "/" + itemPath;

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (isProtectedItemName(itemName)) return sendResponse(req, 403, "text/plain", "Cannot access protected items");

  StorageGuard lock(storageMutex_);
  if (!lock) return sendResponse(req, 503, "text/plain", "Storage unavailable");
  if (!SdMan.exists(itemPath.c_str())) return sendResponse(req, 404, "text/plain", "Item not found");

  FsFile file = SdMan.open(itemPath.c_str());
  if (!file) return sendResponse(req, 500, "text/plain", "Failed to open file");
  if (file.isDirectory()) {
    file.close();
    return sendResponse(req, 400, "text/plain", "Path is a directory");
  }

  char nameBuf[128] = {0};
  String filename = "download";
  if (file.getName(nameBuf, sizeof(nameBuf))) filename = nameBuf;
  const String disposition = "attachment; filename=\"" + filename + "\"";
  httpd_resp_set_hdr(req, "Content-Disposition", disposition.c_str());
  httpd_resp_set_type(req, isEpubFile(itemPath) ? "application/epub+zip" : "application/octet-stream");

  std::array<char, HTTP_BODY_CHUNK_SIZE> buffer{};
  while (file.available()) {
    const int count = file.read(reinterpret_cast<uint8_t*>(buffer.data()), buffer.size());
    if (count < 0) {
      file.close();
      return ESP_FAIL;
    }
    if (count == 0) break;
    if (httpd_resp_send_chunk(req, buffer.data(), static_cast<size_t>(count)) != ESP_OK) {
      file.close();
      return ESP_FAIL;
    }
    esp_task_wdt_reset();
  }
  file.close();
  return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t M4FileTransferHttpRoutes::handleUpload(httpd_req_t* req) const {
  if (req->content_len <= 0) return sendResponse(req, 400, "text/plain", "Missing upload body");
  if (static_cast<size_t>(req->content_len) > HTTP_MAX_BODY_SIZE) {
    return sendResponse(req, 413, "text/plain", "Upload too large");
  }

  std::string contentType;
  std::string boundary;
  if (!readHeader(req, "Content-Type", contentType) ||
      !M4HttpRequestParser::extractMultipartBoundary(contentType, boundary)) {
    return sendResponse(req, 400, "text/plain", "Expected multipart upload");
  }

  String uploadPath = "/";
  String queryPath;
  if (queryArg(req, "path", queryPath)) uploadPath = normalizeWebPath(queryPath);

  StorageGuard lock(storageMutex_);
  if (!lock) return sendResponse(req, 503, "text/plain", "Storage unavailable");

  std::array<char, HTTP_BODY_CHUNK_SIZE> chunk{};
  size_t remaining = static_cast<size_t>(req->content_len);
  std::string headerBuffer;
  headerBuffer.reserve(std::min(HTTP_CONTROL_BODY_SIZE, remaining));
  size_t headersEnd = std::string::npos;
  unsigned timeoutRetries = 0;

  while (headersEnd == std::string::npos && remaining > 0) {
    const size_t wanted = std::min(remaining, chunk.size());
    const int received = httpd_req_recv(req, chunk.data(), wanted);
    if (received == HTTPD_SOCK_ERR_TIMEOUT && timeoutRetries++ < 3) {
      esp_task_wdt_reset();
      continue;
    }
    if (received <= 0) return sendResponse(req, 400, "text/plain", "Upload body read failed");
    timeoutRetries = 0;
    headerBuffer.append(chunk.data(), static_cast<size_t>(received));
    remaining -= static_cast<size_t>(received);
    headersEnd = headerBuffer.find("\r\n\r\n");
    if (headersEnd == std::string::npos && headerBuffer.size() > HTTP_CONTROL_BODY_SIZE) {
      return sendResponse(req, 400, "text/plain", "Upload headers too large");
    }
    esp_task_wdt_reset();
  }

  if (headersEnd == std::string::npos) return sendResponse(req, 400, "text/plain", "Malformed multipart upload");

  std::string filename;
  if (!M4HttpRequestParser::extractMultipartFilename(
          std::string_view(headerBuffer.data(), headersEnd + 4), filename) ||
      filename.empty()) {
    return sendResponse(req, 400, "text/plain", "Missing upload filename");
  }

  String filePath = uploadPath;
  if (!filePath.endsWith("/")) filePath += "/";
  filePath += filename.c_str();

  if (SdMan.exists(filePath.c_str())) SdMan.remove(filePath.c_str());

  FsFile file;
  if (!SdMan.openFileForWrite("WEB", filePath, file)) {
    return sendResponse(req, 400, "text/plain", "Failed to create file on SD card");
  }

  const size_t dataStart = headersEnd + 4;
  std::string pending(headerBuffer.data() + dataStart, headerBuffer.size() - dataStart);
  headerBuffer.clear();
  const size_t tailKeep = boundary.size() + 12;
  bool failed = false;

  auto flushSafePrefix = [&]() {
    if (pending.size() <= tailKeep) return true;
    const size_t flushLength = pending.size() - tailKeep;
    if (!writeFileBytes(file, pending.data(), flushLength)) return false;
    pending.erase(0, flushLength);
    return true;
  };

  if (!flushSafePrefix()) failed = true;
  while (!failed && remaining > 0) {
    const size_t wanted = std::min(remaining, chunk.size());
    const int received = httpd_req_recv(req, chunk.data(), wanted);
    if (received == HTTPD_SOCK_ERR_TIMEOUT && timeoutRetries++ < 3) {
      esp_task_wdt_reset();
      continue;
    }
    if (received <= 0) {
      failed = true;
      break;
    }
    timeoutRetries = 0;
    pending.append(chunk.data(), static_cast<size_t>(received));
    remaining -= static_cast<size_t>(received);
    if (!flushSafePrefix()) failed = true;
  }

  const size_t terminal = failed ? std::string::npos : M4HttpRequestParser::findTerminalBoundary(pending, boundary);
  if (terminal == std::string::npos || !writeFileBytes(file, pending.data(), terminal)) failed = true;
  file.close();

  if (failed) {
    SdMan.remove(filePath.c_str());
    return sendResponse(req, 400, "text/plain", "Upload aborted or malformed");
  }

  clearEpubCacheIfNeeded(filePath);
  const String response = "File uploaded successfully: " + String(filename.c_str());
  return sendResponse(req, 200, "text/plain", response);
}

esp_err_t M4FileTransferHttpRoutes::handleCreateFolder(httpd_req_t* req) const {
  std::vector<M4HttpRequestParser::Field> fields;
  if (!readFormFields(req, fields)) return sendResponse(req, 400, "text/plain", "Invalid form data");
  String folderName = formField(fields, "name");
  if (folderName.isEmpty()) return sendResponse(req, 400, "text/plain", "Missing folder name");

  String parentPath = formField(fields, "path");
  if (parentPath.isEmpty()) parentPath = "/";
  parentPath = normalizeWebPath(parentPath);

  String folderPath = parentPath;
  if (!folderPath.endsWith("/")) folderPath += "/";
  folderPath += folderName;

  StorageGuard lock(storageMutex_);
  if (!lock) return sendResponse(req, 503, "text/plain", "Storage unavailable");
  if (SdMan.exists(folderPath.c_str())) return sendResponse(req, 400, "text/plain", "Folder already exists");
  if (!SdMan.mkdir(folderPath.c_str())) return sendResponse(req, 500, "text/plain", "Failed to create folder");
  return sendResponse(req, 200, "text/plain", "Folder created: " + folderName);
}

esp_err_t M4FileTransferHttpRoutes::handleRename(httpd_req_t* req) const {
  std::vector<M4HttpRequestParser::Field> fields;
  if (!readFormFields(req, fields)) return sendResponse(req, 400, "text/plain", "Invalid form data");
  String itemPath = normalizeWebPath(formField(fields, "path"));
  String newName = formField(fields, "name");
  newName.trim();

  if (itemPath.isEmpty() || itemPath == "/") return sendResponse(req, 400, "text/plain", "Invalid path");
  if (newName.isEmpty()) return sendResponse(req, 400, "text/plain", "New name cannot be empty");
  if (newName.indexOf('/') >= 0 || newName.indexOf('\\') >= 0) return sendResponse(req, 400, "text/plain", "Invalid file name");
  if (isProtectedItemName(newName)) return sendResponse(req, 403, "text/plain", "Cannot rename to protected name");

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (isProtectedItemName(itemName)) return sendResponse(req, 403, "text/plain", "Cannot rename protected item");
  if (newName == itemName) return sendResponse(req, 200, "text/plain", "Name unchanged");

  StorageGuard lock(storageMutex_);
  if (!lock) return sendResponse(req, 503, "text/plain", "Storage unavailable");
  if (!SdMan.exists(itemPath.c_str())) return sendResponse(req, 404, "text/plain", "Item not found");

  FsFile file = SdMan.open(itemPath.c_str());
  if (!file) return sendResponse(req, 500, "text/plain", "Failed to open file");
  if (file.isDirectory()) {
    file.close();
    return sendResponse(req, 400, "text/plain", "Only files can be renamed");
  }

  String parentPath = itemPath.substring(0, itemPath.lastIndexOf('/'));
  if (parentPath.isEmpty()) parentPath = "/";
  String newPath = parentPath;
  if (!newPath.endsWith("/")) newPath += "/";
  newPath += newName;

  if (SdMan.exists(newPath.c_str())) {
    file.close();
    return sendResponse(req, 409, "text/plain", "Target already exists");
  }

  clearEpubCacheIfNeeded(itemPath);
  const bool success = file.rename(newPath.c_str());
  file.close();
  return sendResponse(req, success ? 200 : 500, "text/plain", success ? "Renamed successfully" : "Failed to rename file");
}

esp_err_t M4FileTransferHttpRoutes::handleMove(httpd_req_t* req) const {
  std::vector<M4HttpRequestParser::Field> fields;
  if (!readFormFields(req, fields)) return sendResponse(req, 400, "text/plain", "Invalid form data");
  String itemPath = normalizeWebPath(formField(fields, "path"));
  String destPath = normalizeWebPath(formField(fields, "dest"));

  if (itemPath.isEmpty() || itemPath == "/") return sendResponse(req, 400, "text/plain", "Invalid path");
  if (destPath.isEmpty()) return sendResponse(req, 400, "text/plain", "Invalid destination");

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (isProtectedItemName(itemName)) return sendResponse(req, 403, "text/plain", "Cannot move protected item");
  if (destPath != "/") {
    const String destName = destPath.substring(destPath.lastIndexOf('/') + 1);
    if (isProtectedItemName(destName)) return sendResponse(req, 403, "text/plain", "Cannot move into protected folder");
  }

  StorageGuard lock(storageMutex_);
  if (!lock) return sendResponse(req, 503, "text/plain", "Storage unavailable");
  if (!SdMan.exists(itemPath.c_str())) return sendResponse(req, 404, "text/plain", "Item not found");

  FsFile file = SdMan.open(itemPath.c_str());
  if (!file) return sendResponse(req, 500, "text/plain", "Failed to open file");
  if (file.isDirectory()) {
    file.close();
    return sendResponse(req, 400, "text/plain", "Only files can be moved");
  }

  if (!SdMan.exists(destPath.c_str())) {
    file.close();
    return sendResponse(req, 404, "text/plain", "Destination not found");
  }
  FsFile destDir = SdMan.open(destPath.c_str());
  if (!destDir || !destDir.isDirectory()) {
    if (destDir) destDir.close();
    file.close();
    return sendResponse(req, 400, "text/plain", "Destination is not a folder");
  }
  destDir.close();

  String newPath = destPath;
  if (!newPath.endsWith("/")) newPath += "/";
  newPath += itemName;
  if (newPath == itemPath) {
    file.close();
    return sendResponse(req, 200, "text/plain", "Already in destination");
  }
  if (SdMan.exists(newPath.c_str())) {
    file.close();
    return sendResponse(req, 409, "text/plain", "Target already exists");
  }

  clearEpubCacheIfNeeded(itemPath);
  const bool success = file.rename(newPath.c_str());
  file.close();
  return sendResponse(req, success ? 200 : 500, "text/plain", success ? "Moved successfully" : "Failed to move file");
}

esp_err_t M4FileTransferHttpRoutes::handleDelete(httpd_req_t* req) const {
  std::vector<M4HttpRequestParser::Field> fields;
  if (!readFormFields(req, fields)) return sendResponse(req, 400, "text/plain", "Invalid form data");
  String itemPath = formField(fields, "path");
  String itemType = formField(fields, "type");
  if (itemType.isEmpty()) itemType = "file";

  if (itemPath.isEmpty() || itemPath == "/") return sendResponse(req, 400, "text/plain", "Cannot delete root directory");
  if (!itemPath.startsWith("/")) itemPath = "/" + itemPath;
  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (isProtectedItemName(itemName)) return sendResponse(req, 403, "text/plain", "Cannot delete protected items");

  StorageGuard lock(storageMutex_);
  if (!lock) return sendResponse(req, 503, "text/plain", "Storage unavailable");
  if (!SdMan.exists(itemPath.c_str())) return sendResponse(req, 404, "text/plain", "Item not found");

  bool success = false;
  if (itemType == "folder") {
    FsFile dir = SdMan.open(itemPath.c_str());
    if (dir && dir.isDirectory()) {
      FsFile entry = dir.openNextFile();
      if (entry) {
        entry.close();
        dir.close();
        return sendResponse(req, 400, "text/plain", "Folder is not empty. Delete contents first.");
      }
      dir.close();
    }
    success = SdMan.rmdir(itemPath.c_str());
  } else {
    success = SdMan.remove(itemPath.c_str());
  }
  return sendResponse(req, success ? 200 : 500, "text/plain", success ? "Deleted successfully" : "Failed to delete item");
}

esp_err_t M4FileTransferHttpRoutes::handleSettingsPage(httpd_req_t* req) const {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, SettingsPageHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t M4FileTransferHttpRoutes::handleGetSettings(httpd_req_t* req) const {
  const auto settings = getSettingsList();
  httpd_resp_set_type(req, "application/json");
  if (httpd_resp_send_chunk(req, "[", 1) != ESP_OK) return ESP_FAIL;

  char output[512];
  bool seenFirst = false;
  JsonDocument doc;
  for (const auto& setting : settings) {
    if (!setting.key) continue;
    doc.clear();
    doc["key"] = setting.key;
    doc["name"] = setting.name;
    doc["category"] = setting.category;

    switch (setting.type) {
      case SettingType::TOGGLE:
        doc["type"] = "toggle";
        if (setting.valuePtr) doc["value"] = static_cast<int>(SETTINGS.*(setting.valuePtr));
        break;
      case SettingType::ENUM: {
        doc["type"] = "enum";
        if (setting.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(setting.valuePtr));
        } else if (setting.valueGetter) {
          doc["value"] = static_cast<int>(setting.valueGetter());
        }
        JsonArray options = doc["options"].to<JsonArray>();
        for (const auto& option : setting.enumValues) options.add(option);
        break;
      }
      case SettingType::VALUE:
        doc["type"] = "value";
        if (setting.signedValuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(setting.signedValuePtr));
        } else if (setting.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(setting.valuePtr));
        }
        doc["min"] = static_cast<int>(setting.valueRange.min);
        doc["max"] = static_cast<int>(setting.valueRange.max);
        doc["step"] = static_cast<int>(setting.valueRange.step);
        break;
      case SettingType::STRING:
        doc["type"] = "string";
        if (setting.stringGetter) {
          doc["value"] = setting.stringGetter();
        } else if (setting.stringPtr) {
          doc["value"] = setting.stringPtr;
        }
        break;
      default:
        continue;
    }

    const size_t written = serializeJson(doc, output, sizeof(output));
    if (written >= sizeof(output)) continue;
    if (seenFirst && httpd_resp_send_chunk(req, ",", 1) != ESP_OK) return ESP_FAIL;
    seenFirst = true;
    if (httpd_resp_send_chunk(req, output, written) != ESP_OK) return ESP_FAIL;
  }

  if (httpd_resp_send_chunk(req, "]", 1) != ESP_OK) return ESP_FAIL;
  return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t M4FileTransferHttpRoutes::handlePostSettings(httpd_req_t* req) const {
  std::string body;
  if (!recvBody(req, body, HTTP_CONTROL_BODY_SIZE)) return sendResponse(req, 400, "text/plain", "Missing JSON body");

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, body);
  if (error) return sendResponse(req, 400, "text/plain", String("Invalid JSON: ") + error.c_str());

  StorageGuard lock(storageMutex_);
  if (!lock) return sendResponse(req, 503, "text/plain", "Storage unavailable");

  auto settings = getSettingsList();
  int applied = 0;
  for (auto& setting : settings) {
    if (!setting.key || !doc[setting.key].is<JsonVariant>()) continue;
    switch (setting.type) {
      case SettingType::TOGGLE: {
        const int value = doc[setting.key].as<int>() ? 1 : 0;
        if (setting.valuePtr) SETTINGS.*(setting.valuePtr) = value;
        ++applied;
        break;
      }
      case SettingType::ENUM: {
        const int value = doc[setting.key].as<int>();
        if (value >= 0 && value < static_cast<int>(setting.enumValues.size())) {
          if (setting.valuePtr) {
            SETTINGS.*(setting.valuePtr) = static_cast<uint8_t>(value);
          } else if (setting.valueSetter) {
            setting.valueSetter(static_cast<uint8_t>(value));
          }
          ++applied;
        }
        break;
      }
      case SettingType::VALUE: {
        const int value = doc[setting.key].as<int>();
        if (value >= static_cast<int>(setting.valueRange.min) && value <= static_cast<int>(setting.valueRange.max)) {
          if (setting.signedValuePtr) {
            SETTINGS.*(setting.signedValuePtr) = static_cast<int8_t>(value);
          } else if (setting.valuePtr) {
            SETTINGS.*(setting.valuePtr) = static_cast<uint8_t>(value);
          }
          ++applied;
        }
        break;
      }
      case SettingType::STRING: {
        const std::string value = doc[setting.key].as<std::string>();
        if (setting.stringSetter) {
          setting.stringSetter(value);
        } else if (setting.stringPtr && setting.stringMaxLen > 0) {
          strncpy(setting.stringPtr, value.c_str(), setting.stringMaxLen - 1);
          setting.stringPtr[setting.stringMaxLen - 1] = '\0';
        }
        ++applied;
        break;
      }
      default:
        break;
    }
  }

  SETTINGS.saveToFile();
  return sendResponse(req, 200, "text/plain", String("Applied ") + String(applied) + " setting(s)");
}

esp_err_t M4FileTransferHttpRoutes::handleNotFound(httpd_req_t* req) const {
  const String uri = req && req->uri ? String(req->uri) : String();
  if (!uri.isEmpty() && uri != "/favicon.ico" && uri != "/robots.txt" && !uri.endsWith(".png") &&
      !uri.endsWith(".jpg") && !uri.endsWith(".gif") && !uri.endsWith(".css") && !uri.endsWith(".js")) {
    Serial.printf("[%lu] [HTTPD] 404 Not Found: %s\n", millis(), uri.c_str());
  }
  return sendResponse(req, 404, "text/plain", "Not Found");
}
