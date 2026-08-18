#if defined(CROSSPOINT_MURPHY_M4)

#include "network/M4B3DiscoveryAdvertise.h"

#include <ESPmDNS.h>
#include <mdns.h>

#include <Arduino.h>

#include "util/M4B3Discovery.h"

namespace M4B3DiscoveryAdvertise {
namespace {

bool gAdvertised = false;
uint32_t gAdds = 0;
uint32_t gRemoves = 0;
uint32_t gErrors = 0;

void addService() {
  mdns_txt_item_t txt[] = {
      {const_cast<char*>(M4B3Discovery::kTxtProtoKey), const_cast<char*>(M4B3Discovery::kTxtProtoVal)},
      {const_cast<char*>(M4B3Discovery::kTxtRoleKey), const_cast<char*>(M4B3Discovery::kTxtRoleVal)},
  };
  // begin() fails if a responder is already running (web/keyboard). That is
  // fine: add the narrowly scoped Browser Bridge service onto the existing one.
  (void)MDNS.begin(M4B3Discovery::kHostname);
  const esp_err_t err = mdns_service_add(M4B3Discovery::kInstanceName, "_m4b3", "_tcp", M4B3Discovery::kPort, txt, 2);
  if (err == ESP_OK) {
    gAdvertised = true;
    gAdds++;
    Serial.printf("[%lu] [M4B3] mdns add %s.%s instance=%s port=%u\n", millis(), M4B3Discovery::kServiceType,
                  "local", M4B3Discovery::kInstanceName, static_cast<unsigned>(M4B3Discovery::kPort));
    return;
  }
  // Already present after a previous add, or a shared responder owns the type.
  if (MDNS.addService(M4B3Discovery::kService, M4B3Discovery::kProto, M4B3Discovery::kPort)) {
    (void)MDNS.addServiceTxt(M4B3Discovery::kService, M4B3Discovery::kProto, M4B3Discovery::kTxtProtoKey,
                             M4B3Discovery::kTxtProtoVal);
    (void)MDNS.addServiceTxt(M4B3Discovery::kService, M4B3Discovery::kProto, M4B3Discovery::kTxtRoleKey,
                             M4B3Discovery::kTxtRoleVal);
    gAdvertised = true;
    gAdds++;
    return;
  }
  gErrors++;
}

void removeService() {
  const esp_err_t err = mdns_service_remove("_m4b3", "_tcp");
  if (err != ESP_OK) gErrors++;
  else gRemoves++;
  gAdvertised = false;
  Serial.printf("[%lu] [M4B3] mdns remove %s err=%d\n", millis(), M4B3Discovery::kServiceType, static_cast<int>(err));
}

}  // namespace

void sync(bool wantAdvertise, const char* bindIp) {
  const bool want = wantAdvertise && M4B3Discovery::advertiseAllowed(true, true, bindIp);
  if (want) {
    if (!gAdvertised) addService();
    return;
  }
  if (gAdvertised) removeService();
}

bool advertised() { return gAdvertised; }
uint32_t adds() { return gAdds; }
uint32_t removes() { return gRemoves; }
uint32_t errors() { return gErrors; }

}  // namespace M4B3DiscoveryAdvertise

#endif
