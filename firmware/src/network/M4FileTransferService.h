#pragma once

#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <memory>
#include <string>

class DNSServer;
class M4FileTransferAuxiliaryServer;
class M4FileTransferHttpRoutes;

class M4FileTransferService final {
 public:
  enum class WebServerStartResult {
    Started,
    AllocationFailed,
    StartupFailed,
  };

  // Transitional P1B Activity compatibility. In P1C this callback pumps only
  // WebSocket/UDP auxiliary work; browser HTTP requests are owned by httpd.
  using AbortCheck = bool (*)(void* context);

  M4FileTransferService();
  ~M4FileTransferService();

  M4FileTransferService(const M4FileTransferService&) = delete;
  M4FileTransferService& operator=(const M4FileTransferService&) = delete;

  bool beginAccessPoint(const char* ssid, const char* password, int channel, int maxConnections,
                        const char* hostname, std::string& connectedIp);
  bool beginStationMdns(const char* hostname);
  WebServerStartResult beginWebServer();

  void processDns();
  void pollAuxiliary();
  bool handleWebClients(int maxIters, unsigned long budgetMs, AbortCheck abortCheck = nullptr,
                        void* abortContext = nullptr);

  bool webServerRunning() const { return httpServer != nullptr; }
  bool hasDnsServer() const { return static_cast<bool>(dnsServer); }

  void stopWebServer();
  void stopForSetupError(bool isApMode);
  void stop(bool isApMode);

 private:
  using HttpHandler = esp_err_t (*)(httpd_req_t* req);

  httpd_handle_t httpServer = nullptr;
  std::unique_ptr<M4FileTransferHttpRoutes> httpRoutes;
  std::unique_ptr<M4FileTransferAuxiliaryServer> auxiliaryServer;
  std::unique_ptr<DNSServer> dnsServer;
  SemaphoreHandle_t storageMutex = nullptr;
  bool mdnsRunning = false;

  bool beginMdns(const char* hostname);
  void stopDiscovery();
  bool registerUri(const char* uri, httpd_method_t method, HttpHandler handler);

  static M4FileTransferService* fromRequest(httpd_req_t* req);
  static esp_err_t rootHandler(httpd_req_t* req);
  static esp_err_t filesHandler(httpd_req_t* req);
  static esp_err_t statusHandler(httpd_req_t* req);
  static esp_err_t fileListHandler(httpd_req_t* req);
  static esp_err_t downloadHandler(httpd_req_t* req);
  static esp_err_t uploadHandler(httpd_req_t* req);
  static esp_err_t mkdirHandler(httpd_req_t* req);
  static esp_err_t renameHandler(httpd_req_t* req);
  static esp_err_t moveHandler(httpd_req_t* req);
  static esp_err_t deleteHandler(httpd_req_t* req);
  static esp_err_t settingsPageHandler(httpd_req_t* req);
  static esp_err_t settingsGetHandler(httpd_req_t* req);
  static esp_err_t settingsPostHandler(httpd_req_t* req);
};
