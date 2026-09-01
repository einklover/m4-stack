#include "network/M4FileTransferService.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include <cstring>
#include <new>

#include "network/CrossPointWebServer.h"
#include "network/NetworkConstants.h"

M4FileTransferService::~M4FileTransferService() {
  if (webServer) {
    if (webServer->isRunning()) webServer->stop();
    webServer.reset();
  }
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

M4FileTransferService::WebServerStartResult M4FileTransferService::beginWebServer() {
  if (webServer && webServer->isRunning()) return WebServerStartResult::Started;
  webServer.reset();
  webServer.reset(new (std::nothrow) CrossPointWebServer());
  if (!webServer) return WebServerStartResult::AllocationFailed;

  webServer->begin();
  if (!webServer->isRunning()) {
    webServer.reset();
    return WebServerStartResult::StartupFailed;
  }
  return WebServerStartResult::Started;
}

void M4FileTransferService::processDns() {
  if (dnsServer) dnsServer->processNextRequest();
}

bool M4FileTransferService::webServerRunning() const { return webServer && webServer->isRunning(); }

bool M4FileTransferService::handleWebClients(const int maxIters, const unsigned long budgetMs,
                                             const AbortCheck abortCheck, void* abortContext) {
  if (!webServerRunning()) return false;

  esp_task_wdt_reset();
  const unsigned long t0 = millis();
  for (int i = 0; i < maxIters && webServerRunning(); ++i) {
    webServer->handleClient();
    if (static_cast<unsigned long>(millis() - t0) >= budgetMs) break;
    if ((i & 0x3) == 0x3) {
      yield();
      esp_task_wdt_reset();
      if (abortCheck && abortCheck(abortContext)) return true;
    }
  }
  return false;
}

void M4FileTransferService::stopWebServer() {
  const unsigned long stopStarted = millis();
  const bool hadServer = static_cast<bool>(webServer);
  if (webServer) {
    if (webServer->isRunning()) webServer->stop();
    webServer.reset();
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
