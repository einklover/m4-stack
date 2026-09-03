#include "network/M4FileTransferService.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstring>
#include <memory>
#include <new>

#include "network/M4FileTransferAuxiliaryServer.h"
#include "network/M4FileTransferHttpRoutes.h"
#include "network/NetworkConstants.h"
#include "qemu/M4QemuNet.h"

class M4FileTransferService::HttpRuntime final {
 public:
  httpd_handle_t httpServer = nullptr;
  std::unique_ptr<M4FileTransferHttpRoutes> httpRoutes;
  std::unique_ptr<M4FileTransferAuxiliaryServer> auxiliaryServer;
  SemaphoreHandle_t storageMutex = nullptr;
};

namespace {
using RouteMethod = esp_err_t (M4FileTransferHttpRoutes::*)(httpd_req_t*) const;

bool registerUri(const httpd_handle_t server, const char* uri, const httpd_method_t method,
                 esp_err_t (*handler)(httpd_req_t*), void* context) {
  if (!server || !uri || !handler || !context) return false;
  httpd_uri_t descriptor{};
  descriptor.uri = uri;
  descriptor.method = method;
  descriptor.handler = handler;
  descriptor.user_ctx = context;
  return httpd_register_uri_handler(server, &descriptor) == ESP_OK;
}

template <RouteMethod Method>
esp_err_t routeHandler(httpd_req_t* req) {
  auto* routes = req ? static_cast<M4FileTransferHttpRoutes*>(req->user_ctx) : nullptr;
  return routes ? (routes->*Method)(req) : ESP_FAIL;
}
}  // namespace

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

  if (memoryAccount.acquireDns()) {
    dnsServer.reset(new (std::nothrow) DNSServer());
    if (dnsServer) {
      dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
      dnsServer->start(NetworkConstants::DNS_PORT, "*", apIP);
    } else {
      memoryAccount.releaseDns();
      Serial.printf("[%lu] [WEBACT] WARN: no memory for captive DNS; direct IP still works\n", millis());
    }
  } else {
    Serial.printf("[%lu] [WEBACT] WARN: captive DNS governance admission denied; direct IP still works\n",
                  millis());
  }
  return true;
}

M4FileTransferService::WebServerStartResult M4FileTransferService::beginWebServer() {
  if (webServerRunning()) return WebServerStartResult::Started;

  const wifi_mode_t wifiMode = WiFi.getMode();
  const bool staReady = M4QemuNet::staConnected() || ((wifiMode & WIFI_MODE_STA) && WiFi.status() == WL_CONNECTED);
  const bool apReady = (wifiMode & WIFI_MODE_AP) != 0;
  if (!staReady && !apReady) {
    Serial.printf("[%lu] [HTTPD] refusing start: no valid network (mode=%d status=%d qemu_eth=%d)\n", millis(),
                  wifiMode, WiFi.status(), m4QemuNetIsUp() ? 1 : 0);
    return WebServerStartResult::StartupFailed;
  }

  if (!memoryAccount.acquireHttpRuntime()) {
    Serial.printf("[%lu] [HTTPD] refusing start: memory governance admission denied\n", millis());
    return WebServerStartResult::AllocationFailed;
  }

  httpRuntime.reset(new (std::nothrow) HttpRuntime());
  if (!httpRuntime) {
    memoryAccount.releaseHttpRuntime();
    return WebServerStartResult::AllocationFailed;
  }
  auto& runtime = *httpRuntime;

  runtime.storageMutex = xSemaphoreCreateMutex();
  if (!runtime.storageMutex) {
    stopWebServer();
    return WebServerStartResult::AllocationFailed;
  }

  runtime.httpRoutes.reset(new (std::nothrow) M4FileTransferHttpRoutes(runtime.storageMutex));
  runtime.auxiliaryServer.reset(new (std::nothrow) M4FileTransferAuxiliaryServer(runtime.storageMutex));
  if (!runtime.httpRoutes || !runtime.auxiliaryServer) {
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
  if (httpd_start(&runtime.httpServer, &config) != ESP_OK || !runtime.httpServer) {
    runtime.httpServer = nullptr;
    stopWebServer();
    return WebServerStartResult::StartupFailed;
  }

  void* routeContext = runtime.httpRoutes.get();
  const bool routesRegistered =
      registerUri(runtime.httpServer, "/", HTTP_GET, routeHandler<&M4FileTransferHttpRoutes::handleRoot>, routeContext) &&
      registerUri(runtime.httpServer, "/files", HTTP_GET, routeHandler<&M4FileTransferHttpRoutes::handleFileList>, routeContext) &&
      registerUri(runtime.httpServer, "/api/status", HTTP_GET, routeHandler<&M4FileTransferHttpRoutes::handleStatus>, routeContext) &&
      registerUri(runtime.httpServer, "/api/files", HTTP_GET, routeHandler<&M4FileTransferHttpRoutes::handleFileListData>, routeContext) &&
      registerUri(runtime.httpServer, "/download", HTTP_GET, routeHandler<&M4FileTransferHttpRoutes::handleDownload>, routeContext) &&
      registerUri(runtime.httpServer, "/upload", HTTP_POST, routeHandler<&M4FileTransferHttpRoutes::handleUpload>, routeContext) &&
      registerUri(runtime.httpServer, "/mkdir", HTTP_POST, routeHandler<&M4FileTransferHttpRoutes::handleCreateFolder>, routeContext) &&
      registerUri(runtime.httpServer, "/rename", HTTP_POST, routeHandler<&M4FileTransferHttpRoutes::handleRename>, routeContext) &&
      registerUri(runtime.httpServer, "/move", HTTP_POST, routeHandler<&M4FileTransferHttpRoutes::handleMove>, routeContext) &&
      registerUri(runtime.httpServer, "/delete", HTTP_POST, routeHandler<&M4FileTransferHttpRoutes::handleDelete>, routeContext) &&
      registerUri(runtime.httpServer, "/settings", HTTP_GET, routeHandler<&M4FileTransferHttpRoutes::handleSettingsPage>, routeContext) &&
      registerUri(runtime.httpServer, "/api/settings", HTTP_GET, routeHandler<&M4FileTransferHttpRoutes::handleGetSettings>, routeContext) &&
      registerUri(runtime.httpServer, "/api/settings", HTTP_POST, routeHandler<&M4FileTransferHttpRoutes::handlePostSettings>, routeContext);
  if (!routesRegistered) {
    Serial.printf("[%lu] [HTTPD] failed to register one or more file-transfer routes\n", millis());
    stopWebServer();
    return WebServerStartResult::StartupFailed;
  }

  if (!runtime.auxiliaryServer->begin()) {
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
  if (httpRuntime && httpRuntime->auxiliaryServer) httpRuntime->auxiliaryServer->poll();
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

bool M4FileTransferService::webServerRunning() const {
  return httpRuntime && httpRuntime->httpServer != nullptr;
}

void M4FileTransferService::stopWebServer() {
  const unsigned long stopStarted = millis();
  const bool hadServer = webServerRunning();

  if (!httpRuntime) {
    memoryAccount.releaseHttpRuntime();
    Serial.printf("[%lu] [WEBACT] server_stop_ms=%lu had_server=0\n", millis(),
                  static_cast<unsigned long>(millis() - stopStarted));
    return;
  }

  auto& runtime = *httpRuntime;
  // Release any WebSocket-held storage lock before stopping the HTTP task.
  if (runtime.auxiliaryServer) {
    runtime.auxiliaryServer->stop();
    runtime.auxiliaryServer.reset();
  }

  if (runtime.httpServer) {
    httpd_stop(runtime.httpServer);
    runtime.httpServer = nullptr;
  }

  runtime.httpRoutes.reset();
  if (runtime.storageMutex) {
    vSemaphoreDelete(runtime.storageMutex);
    runtime.storageMutex = nullptr;
  }
  httpRuntime.reset();
  memoryAccount.releaseHttpRuntime();

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
  memoryAccount.releaseDns();
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
