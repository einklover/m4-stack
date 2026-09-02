#pragma once

#include <SDCardManager.h>
#include <WebSocketsServer.h>
#include <WiFiUdp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstddef>
#include <memory>
#include <string>

class M4FileTransferAuxiliaryServer final {
 public:
  struct WsUploadStatus {
    bool inProgress = false;
    size_t received = 0;
    size_t total = 0;
    std::string filename;
    std::string lastCompleteName;
    size_t lastCompleteSize = 0;
    unsigned long lastCompleteAt = 0;
  };

  explicit M4FileTransferAuxiliaryServer(SemaphoreHandle_t storageMutex);
  ~M4FileTransferAuxiliaryServer();

  M4FileTransferAuxiliaryServer(const M4FileTransferAuxiliaryServer&) = delete;
  M4FileTransferAuxiliaryServer& operator=(const M4FileTransferAuxiliaryServer&) = delete;

  bool begin();
  void poll();
  void stop();
  bool running() const { return running_; }
  WsUploadStatus uploadStatus() const;

 private:
  static constexpr uint16_t WS_PORT = 81;
  static constexpr uint16_t LOCAL_UDP_PORT = 8134;

  static M4FileTransferAuxiliaryServer* wsInstance_;
  static void wsEventCallback(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
  void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);

  bool acquireStorage();
  void releaseStorage();
  void abortUpload(bool deletePartial);
  void clearEpubCacheIfNeeded(const String& filePath) const;

  SemaphoreHandle_t storageMutex_ = nullptr;
  bool storageLocked_ = false;
  std::unique_ptr<WebSocketsServer> wsServer_;
  WiFiUDP udp_;
  bool udpActive_ = false;
  bool running_ = false;

  FsFile wsUploadFile_;
  String wsUploadFileName_;
  String wsUploadPath_;
  size_t wsUploadSize_ = 0;
  size_t wsUploadReceived_ = 0;
  unsigned long wsUploadStartTime_ = 0;
  bool wsUploadInProgress_ = false;
  String wsLastCompleteName_;
  size_t wsLastCompleteSize_ = 0;
  unsigned long wsLastCompleteAt_ = 0;
};
