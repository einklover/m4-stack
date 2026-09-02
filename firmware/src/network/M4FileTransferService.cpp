#include "network/M4FileTransferService.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>

#include <cstring>
#include <new>

#include "network/M4FileTransferAuxiliaryServer.h"
#include "network/M4FileTransferHttpRoutes.h"
#include "network/NetworkConstants.h"
#include "qemu/M4QemuNet.h"

M4FileTransferService::M4FileTransferService() = default;

M4FileTransferService::~M4FileTransferService() {
  stopWebServer();
  stopDiscovery();
}

bool M4FileTransferService::beginMdns(const char* hostname) {
  if (mdnsRunning) {
    MDNS.end();
    mdnsRunning = false;
  }
  if (!hostname || !*hostname) return false;
  mdnsRunning = MDNS.begin(hostname);
  if (mdnsRunning) {
    Serial.printf("[%lu] [WEBACT] mDNS started: http://%s.local/\n", millis(), hostname);
  }
  return mdnsRunning;
}

bool M4FileTransferService::beginStationMdns(const char* hostname) { return beginMdns(hostname); }

bool M4FileTransferService::beginAccessPoint(const char* ssid, const char* password, const int channel,
                                             const int maxConnections, const char* hostname,
                                             std::string& connectedIp) {
  WiFi.mode(WIFI_AP);
  delay(100);
  const bool hasPassword = password && strlen(password) >= 8;
  const bool apStarted = hasPassword ? WiFi.softAP(ssid, password, channel, false, maxConnections)
                                     : WiFi.softAP(ssid, nullptr, channel, false, maxConnections);
  if (!apStarted) return false;

  delay(100);
  const IPAddress apIP = WiFi.softAPIP();
  char ipStr[16];
  snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", apIP[0], apIP[1], apIP[2], apIP[3]);
  connectedIp = ipStr;

  beginMdns(hostname);

  dnsServer.reset(new (std::nothrow) DNSServer());
  if (dnsServer) {
    dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer->start(NetworkConstants::DNS_PORT, "*", apIP);
  } else {
    Serial.printf("[%lu] [WEBACT] WARN: no memory for captive DNS; direct IP still works\n", millis());
  }
  return true;
}

bool M4FileTransferService::registerUri(const char* uri, const httpd_method_t method, const HttpHandler handler) {
  if (!httpServer || !uri || !handler) return false;
  httpd_uri_t descriptor{};
  descriptor.uri = uri;
  descriptor.method = method;
  descriptor.handler = handler;
  descriptor.user_ctx = this;
  return httpd_register_uri_handler(httpServer, &descriptor) == ESP_OK;
}

M4FileTransferService::WebServerStartResult M4FileTransferService::beginWebServer() {
  if (httpServer) return WebServerStartResult::Started;

  const wifi_mode_t wifiMode = WiFi.getMode();
  const bool staReady = M4QemuNet::staConnected() || ((wifiMode & WIFI_MODE_STA) && WiFi.status() == WL_CONNECTED);
  const bool apReady = (wifiMode & WIFI_MODE_AP) != 0;
  if (!staReady && !apReady) {
    Serial.printf("[%lu] [HTTPD] refusing start: no valid network (mode=%d status=%d qemu_eth=%d)\n", millis(),
                  wifiMode, WiFi.status(), m4QemuNetIsUp() ? 1 : 0);
    return WebServerStartResult::StartupFailed;
  }

  storageMutex = xSemaphoreCreateMutex();
  if (!storageMutex) return WebServerStartResult::AllocationFailed;

  httpRoutes.reset(new (std::nothrow) M4FileTransferHttpRoutes(storageMutex));
  auxiliaryServer.reset(new (std::nothrow) M4FileTransferAuxiliaryServer(storageMutex));
  if (!httpRoutes || !auxiliaryServer) {
    stopWebServer();
    return WebServerStartResult::AllocationFailed;
  }

  WiFi.setSleep(false);

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_uri_handlers = 16;
  config.stack_size = 8192;
  config.lru_purge_enable = true;
  config.recv_wait_timeout = 5;
  config.send_wait_timeout = 5;

  Serial.printf("[%lu] [HTTPD] starting esp_http_server on port %u\n", millis(), config.server_port);
  if (httpd_start(&httpServer, &config) != ESP_OK || !httpServer) {
    httpServer = nullptr;
    stopWebServer();
    return WebServerStartResult::StartupFailed;
  }

  const bool routesRegistered =
      registerUri("/", HTTP_GET, rootHandler) && registerUri("/files", HTTP_GET, filesHandler) &&
      registerUri("/api/status", HTTP_GET, statusHandler) && registerUri("/api/files", HTTP_GET, fileListHandler) &&
      registerUri("/download", HTTP_GET, downloadHandler) && registerUri("/upload", HTTP_POST, uploadHandler) &&
      registerUri("/mkdir", HTTP_POST, mkdirHandler) && registerUri("/rename", HTTP_POST, renameHandler) &&
      registerUri("/move", HTTP_POST, moveHandler) && registerUri("/delete", HTTP_POST, deleteHandler) &&
      registerUri("/settings", HTTP_GET, settingsPageHandler) &&
      registerUri("/api/settings", HTTP_GET, settingsGetHandler) &&
      registerUri("/api/settings", HTTP_POST, settingsPostHandler);
  if (!routesRegistered) {
    Serial.printf("[%lu] [HTTPD] failed to register one or more file-transfer routes\n", millis());
    stopWebServer();
    return WebServerStartResult::StartupFailed;
  }

  if (!auxiliaryServer->begin()) {
    stopWebServer();
    return WebServerStartResult::AllocationFailed;
  }

  Serial.printf("[%lu] [HTTPD] esp_http_server ready; HTTP requests now run on the server task\n", millis());
  return WebServerStartResult::Started;
}

void M4FileTransferService::processDns() {
  if (dnsServer) dnsServer->processNextRequest();
}

void M4FileTransferService::pollAuxiliary() {
  if (auxiliaryServer) auxiliaryServer->poll();
}

bool M4FileTransferService::handleWebClients(const int maxIters, const unsigned long budgetMs,
                                             const AbortCheck abortCheck, void* abortContext) {
  (void)maxIters;
  (void)budgetMs;
  // P1C compatibility seam: browser HTTP is serviced by esp_http_server's own
  // task. The Activity pump only advances the retained WebSocket/UDP channels.
  pollAuxiliary();
  return abortCheck && abortCheck(abortContext);
}

void M4FileTransferService::stopWebServer() {
  const unsigned long stopStarted = millis();
  const bool hadServer = httpServer != nullptr;

  // Release any WebSocket-held storage lock before stopping the HTTP task.
  if (auxiliaryServer) {
    auxiliaryServer->stop();
    auxiliaryServer.reset();
  }

  if (httpServer) {
    httpd_stop(httpServer);
    httpServer = nullptr;
  }

  httpRoutes.reset();
  if (storageMutex) {
    vSemaphoreDelete(storageMutex);
    storageMutex = nullptr;
  }

  Serial.printf("[%lu] [WEBACT] server_stop_ms=%lu had_server=%d\n", millis(),
                static_cast<unsigned long>(millis() - stopStarted), hadServer ? 1 : 0);
}

void M4FileTransferService::stopDiscovery() {
  MDNS.end();
  mdnsRunning = false;
  if (dnsServer) {
    dnsServer->stop();
    dnsServer.reset();
  }
}

void M4FileTransferService::stopForSetupError(const bool isApMode) {
  stopWebServer();
  stopDiscovery();
#if !defined(M4_QEMU_PLUGIN_DEBUG) || !M4_QEMU_PLUGIN_DEBUG
  if (isApMode) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
  }
#endif
}

void M4FileTransferService::stop(const bool isApMode) {
  stopWebServer();
  stopDiscovery();
  delay(200);
#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG
  if (isApMode) WiFi.softAPdisconnect(true);
#else
  if (isApMode) {
    WiFi.softAPdisconnect(true);
  } else {
    WiFi.disconnect(false);
  }
  delay(300);
  WiFi.mode(WIFI_OFF);
  delay(300);
#endif
}

M4FileTransferService* M4FileTransferService::fromRequest(httpd_req_t* req) {
  return req ? static_cast<M4FileTransferService*>(req->user_ctx) : nullptr;
}

esp_err_t M4FileTransferService::rootHandler(httpd_req_t* req) {
  auto* service = fromRequest(req);
  return service && service->httpRoutes ? service->httpRoutes->handleRoot(req) : ESP_FAIL;
}

esp_err_t M4FileTransferService::filesHandler(httpd_req_t* req) {
  auto* service = fromRequest(req);
  return service && service->httpRoutes ? service->httpRoutes->handleFileList(req) : ESP_FAIL;
}

esp_err_t M4FileTransferService::statusHandler(httpd_req_t* req) {
  auto* service = fromRequest(req);
  return service && service->httpRoutes ? service->httpRoutes->handleStatus(req) : ESP_FAIL;
}

esp_err_t M4FileTransferService::fileListHandler(httpd_req_t* req) {
  auto* service = fromRequest(req);
  return service && service->httpRoutes ? service->httpRoutes->handleFileListData(req) : ESP_FAIL;
}

esp_err_t M4FileTransferService::downloadHandler(httpd_req_t* req) {
  auto* service = fromRequest(req);
  return service && service->httpRoutes ? service->httpRoutes->handleDownload(req) : ESP_FAIL;
}

esp_err_t M4FileTransferService::uploadHandler(httpd_req_t* req) {
  auto* service = fromRequest(req);
  return service && service->httpRoutes ? service->httpRoutes->handleUpload(req) : ESP_FAIL;
}

esp_err_t M4FileTransferService::mkdirHandler(httpd_req_t* req) {
  auto* service = fromRequest(req);
  return service && service->httpRoutes ? service->httpRoutes->handleCreateFolder(req) : ESP_FAIL;
}

esp_err_t M4FileTransferService::renameHandler(httpd_req_t* req) {
  auto* service = fromRequest(req);
  return service && service->httpRoutes ? service->httpRoutes->handleRename(req) : ESP_FAIL;
}

esp_err_t M4FileTransferService::moveHandler(httpd_req_t* req) {
  auto* service = fromRequest(req);
  return service && service->httpRoutes ? service->httpRoutes->handleMove(req) : ESP_FAIL;
}

esp_err_t M4FileTransferService::deleteHandler(httpd_req_t* req) {
  auto* service = fromRequest(req);
  return service && service->httpRoutes ? service->httpRoutes->handleDelete(req) : ESP_FAIL;
}

esp_err_t M4FileTransferService::settingsPageHandler(httpd_req_t* req) {
  auto* service = fromRequest(req);
  return service && service->httpRoutes ? service->httpRoutes->handleSettingsPage(req) : ESP_FAIL;
}

esp_err_t M4FileTransferService::settingsGetHandler(httpd_req_t* req) {
  auto* service = fromRequest(req);
  return service && service->httpRoutes ? service->httpRoutes->handleGetSettings(req) : ESP_FAIL;
}

esp_err_t M4FileTransferService::settingsPostHandler(httpd_req_t* req) {
  auto* service = fromRequest(req);
  return service && service->httpRoutes ? service->httpRoutes->handlePostSettings(req) : ESP_FAIL;
}
