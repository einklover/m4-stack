#pragma once

#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstddef>

class M4FileTransferHttpRoutes final {
 public:
  static constexpr size_t HTTP_BODY_CHUNK_SIZE = 4096;
  static constexpr size_t HTTP_MAX_BODY_SIZE = 512u * 1024u * 1024u;
  static constexpr size_t HTTP_CONTROL_BODY_SIZE = 16u * 1024u;

  explicit M4FileTransferHttpRoutes(SemaphoreHandle_t storageMutex) : storageMutex_(storageMutex) {}

  esp_err_t handleRoot(httpd_req_t* req) const;
  esp_err_t handleFileList(httpd_req_t* req) const;
  esp_err_t handleStatus(httpd_req_t* req) const;
  esp_err_t handleFileListData(httpd_req_t* req) const;
  esp_err_t handleDownload(httpd_req_t* req) const;
  esp_err_t handleUpload(httpd_req_t* req) const;
  esp_err_t handleCreateFolder(httpd_req_t* req) const;
  esp_err_t handleRename(httpd_req_t* req) const;
  esp_err_t handleMove(httpd_req_t* req) const;
  esp_err_t handleDelete(httpd_req_t* req) const;
  esp_err_t handleSettingsPage(httpd_req_t* req) const;
  esp_err_t handleGetSettings(httpd_req_t* req) const;
  esp_err_t handlePostSettings(httpd_req_t* req) const;
  esp_err_t handleNotFound(httpd_req_t* req) const;

 private:
  SemaphoreHandle_t storageMutex_ = nullptr;
};
