#pragma once

#include <memory>
#include <string>

class CrossPointWebServer;
class DNSServer;

class M4FileTransferService final {
 public:
  enum class WebServerStartResult {
    Started,
    AllocationFailed,
    StartupFailed,
  };

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
  bool handleWebClients(int maxIters, unsigned long budgetMs, AbortCheck abortCheck = nullptr,
                        void* abortContext = nullptr);

  bool webServerRunning() const;
  bool hasDnsServer() const { return static_cast<bool>(dnsServer); }

  void stopWebServer();
  void stopForSetupError(bool isApMode);
  void stop(bool isApMode);

 private:
  std::unique_ptr<CrossPointWebServer> webServer;
  std::unique_ptr<DNSServer> dnsServer;
  bool mdnsRunning = false;

  bool beginMdns(const char* hostname);
  void stopDiscovery();
};
