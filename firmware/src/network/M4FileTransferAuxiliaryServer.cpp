#include "network/M4FileTransferAuxiliaryServer.h"

#include <Arduino.h>
#include <Epub.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include <cstring>
#include <new>

#include "apps/providers/M4LanVisitorStore.h"
#include "qemu/M4QemuNet.h"
#include "util/StringUtils.h"

namespace {
void noteTransferVisitorIp(const IPAddress& ip) {
  if (!M4QemuNet::staConnected()) return;
  if (ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0) return;
  const String ssid = M4QemuNet::ssidString();
  if (ssid.isEmpty()) return;
  char ipBuf[16];
  snprintf(ipBuf, sizeof(ipBuf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  M4LanVisitorStore::note(ssid.c_str(), ipBuf);
}
}  // namespace

M4FileTransferAuxiliaryServer* M4FileTransferAuxiliaryServer::wsInstance_ = nullptr;

M4FileTransferAuxiliaryServer::M4FileTransferAuxiliaryServer(const SemaphoreHandle_t storageMutex)
    : storageMutex_(storageMutex) {}

M4FileTransferAuxiliaryServer::~M4FileTransferAuxiliaryServer() { stop(); }

bool M4FileTransferAuxiliaryServer::begin() {
  if (running_) return true;

  wsServer_.reset(new (std::nothrow) WebSocketsServer(WS_PORT));
  if (!wsServer_) {
    Serial.printf("[%lu] [WEB-AUX] failed to allocate WebSocket server\n", millis());
    return false;
  }

  wsInstance_ = this;
  wsServer_->begin();
  wsServer_->onEvent(wsEventCallback);

  udpActive_ = udp_.begin(LOCAL_UDP_PORT);
  Serial.printf("[%lu] [WEB-AUX] WebSocket server started on %u; discovery UDP %s on %u\n", millis(), WS_PORT,
                udpActive_ ? "enabled" : "failed", LOCAL_UDP_PORT);
  running_ = true;
  return true;
}

void M4FileTransferAuxiliaryServer::stop() {
  if (wsUploadInProgress_ || storageLocked_) abortUpload(true);

  if (wsServer_) {
    wsServer_->close();
    wsServer_.reset();
  }
  if (wsInstance_ == this) wsInstance_ = nullptr;

  if (udpActive_) {
    udp_.stop();
    udpActive_ = false;
  }
  running_ = false;
}

void M4FileTransferAuxiliaryServer::poll() {
  if (!running_) return;

  if (wsServer_) wsServer_->loop();

  if (!udpActive_) return;
  const int packetSize = udp_.parsePacket();
  if (packetSize <= 0) return;

  char buffer[16];
  const int len = udp_.read(buffer, sizeof(buffer) - 1);
  if (len <= 0) return;
  buffer[len] = '\0';
  if (strcmp(buffer, "hello") != 0) return;

  noteTransferVisitorIp(udp_.remoteIP());
  String hostname = WiFi.getHostname();
  if (hostname.isEmpty()) hostname = "crosspoint";
  const String message = "crosspoint (on " + hostname + ");" + String(WS_PORT);
  udp_.beginPacket(udp_.remoteIP(), udp_.remotePort());
  udp_.write(reinterpret_cast<const uint8_t*>(message.c_str()), message.length());
  udp_.endPacket();
}

M4FileTransferAuxiliaryServer::WsUploadStatus M4FileTransferAuxiliaryServer::uploadStatus() const {
  WsUploadStatus status;
  status.inProgress = wsUploadInProgress_;
  status.received = wsUploadReceived_;
  status.total = wsUploadSize_;
  status.filename = wsUploadFileName_.c_str();
  status.lastCompleteName = wsLastCompleteName_.c_str();
  status.lastCompleteSize = wsLastCompleteSize_;
  status.lastCompleteAt = wsLastCompleteAt_;
  return status;
}

bool M4FileTransferAuxiliaryServer::acquireStorage() {
  if (storageLocked_) return true;
  if (!storageMutex_) return false;
  if (xSemaphoreTake(storageMutex_, portMAX_DELAY) != pdTRUE) return false;
  storageLocked_ = true;
  return true;
}

void M4FileTransferAuxiliaryServer::releaseStorage() {
  if (!storageLocked_) return;
  storageLocked_ = false;
  xSemaphoreGive(storageMutex_);
}

void M4FileTransferAuxiliaryServer::clearEpubCacheIfNeeded(const String& filePath) const {
  if (StringUtils::checkFileExtension(filePath, ".epub")) {
    Epub(filePath.c_str(), "/.crosspoint").clearCache();
    Serial.printf("[%lu] [WS] Cleared epub cache for: %s\n", millis(), filePath.c_str());
  }
}

void M4FileTransferAuxiliaryServer::abortUpload(const bool deletePartial) {
  String filePath;
  if (!wsUploadPath_.isEmpty() && !wsUploadFileName_.isEmpty()) {
    filePath = wsUploadPath_;
    if (!filePath.endsWith("/")) filePath += "/";
    filePath += wsUploadFileName_;
  }

  if (wsUploadFile_) wsUploadFile_.close();
  if (deletePartial && !filePath.isEmpty()) SdMan.remove(filePath.c_str());

  wsUploadInProgress_ = false;
  wsUploadReceived_ = 0;
  wsUploadSize_ = 0;
  releaseStorage();
}

void M4FileTransferAuxiliaryServer::wsEventCallback(const uint8_t num, const WStype_t type, uint8_t* payload,
                                                    const size_t length) {
  if (wsInstance_) wsInstance_->onWebSocketEvent(num, type, payload, length);
}

void M4FileTransferAuxiliaryServer::onWebSocketEvent(const uint8_t num, const WStype_t type, uint8_t* payload,
                                                     const size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.printf("[%lu] [WS] Client %u disconnected\n", millis(), num);
      if (wsUploadInProgress_ || storageLocked_) abortUpload(true);
      break;

    case WStype_CONNECTED:
      Serial.printf("[%lu] [WS] Client %u connected\n", millis(), num);
      break;

    case WStype_TEXT: {
      const String msg = String(reinterpret_cast<char*>(payload));
      if (!msg.startsWith("START:")) break;

      const int firstColon = msg.indexOf(':', 6);
      const int secondColon = msg.indexOf(':', firstColon + 1);
      if (firstColon <= 0 || secondColon <= 0) {
        if (wsServer_) wsServer_->sendTXT(num, "ERROR:Invalid START format");
        break;
      }

      if (wsUploadInProgress_ || storageLocked_) abortUpload(true);
      if (!acquireStorage()) {
        if (wsServer_) wsServer_->sendTXT(num, "ERROR:Storage busy");
        break;
      }

      wsUploadFileName_ = msg.substring(6, firstColon);
      wsUploadSize_ = static_cast<size_t>(msg.substring(firstColon + 1, secondColon).toInt());
      wsUploadPath_ = msg.substring(secondColon + 1);
      wsUploadReceived_ = 0;
      wsUploadStartTime_ = millis();

      if (!wsUploadPath_.startsWith("/")) wsUploadPath_ = "/" + wsUploadPath_;
      if (wsUploadPath_.length() > 1 && wsUploadPath_.endsWith("/")) {
        wsUploadPath_ = wsUploadPath_.substring(0, wsUploadPath_.length() - 1);
      }

      String filePath = wsUploadPath_;
      if (!filePath.endsWith("/")) filePath += "/";
      filePath += wsUploadFileName_;

      esp_task_wdt_reset();
      if (SdMan.exists(filePath.c_str())) SdMan.remove(filePath.c_str());
      esp_task_wdt_reset();
      if (!SdMan.openFileForWrite("WS", filePath, wsUploadFile_)) {
        if (wsServer_) wsServer_->sendTXT(num, "ERROR:Failed to create file");
        abortUpload(false);
        break;
      }
      esp_task_wdt_reset();

      wsUploadInProgress_ = true;
      if (wsServer_) wsServer_->sendTXT(num, "READY");
      break;
    }

    case WStype_BIN: {
      if (!wsUploadInProgress_ || !wsUploadFile_) {
        if (wsServer_) wsServer_->sendTXT(num, "ERROR:No upload in progress");
        break;
      }

      esp_task_wdt_reset();
      const size_t written = wsUploadFile_.write(payload, length);
      esp_task_wdt_reset();
      if (written != length) {
        if (wsServer_) wsServer_->sendTXT(num, "ERROR:Write failed - disk full?");
        abortUpload(true);
        break;
      }

      wsUploadReceived_ += written;
      static size_t lastProgressSent = 0;
      if (wsUploadReceived_ - lastProgressSent >= 65536 || wsUploadReceived_ >= wsUploadSize_) {
        if (wsServer_) {
          const String progress = "PROGRESS:" + String(wsUploadReceived_) + ":" + String(wsUploadSize_);
          wsServer_->sendTXT(num, progress);
        }
        lastProgressSent = wsUploadReceived_;
      }

      if (wsUploadReceived_ < wsUploadSize_) break;

      wsUploadFile_.close();
      wsUploadInProgress_ = false;
      wsLastCompleteName_ = wsUploadFileName_;
      wsLastCompleteSize_ = wsUploadSize_;
      wsLastCompleteAt_ = millis();

      const unsigned long elapsed = millis() - wsUploadStartTime_;
      const float kbps = elapsed > 0 ? (wsUploadSize_ / 1024.0f) / (elapsed / 1000.0f) : 0.0f;
      Serial.printf("[%lu] [WS] Upload complete: %s (%u bytes in %lu ms, %.1f KB/s)\n", millis(),
                    wsUploadFileName_.c_str(), static_cast<unsigned>(wsUploadSize_), elapsed, kbps);

      String filePath = wsUploadPath_;
      if (!filePath.endsWith("/")) filePath += "/";
      filePath += wsUploadFileName_;
      clearEpubCacheIfNeeded(filePath);

      releaseStorage();
      if (wsServer_) wsServer_->sendTXT(num, "DONE");
      lastProgressSent = 0;
      break;
    }

    default:
      break;
  }
}
