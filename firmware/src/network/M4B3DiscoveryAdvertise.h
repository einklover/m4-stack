#pragma once

// ESP32 mDNS advertisement for the Browser Bridge TCP endpoint.
// Adds/removes only `_m4b3._tcp`. Does not change M4B3 framing, ACK, display,
// or input. Does not call MDNS.end() so other activities can keep a responder.

#if defined(CROSSPOINT_MURPHY_M4)

#include <cstdint>

namespace M4B3DiscoveryAdvertise {

void sync(bool wantAdvertise, const char* bindIp);
bool advertised();
uint32_t adds();
uint32_t removes();
uint32_t errors();

}  // namespace M4B3DiscoveryAdvertise

#endif
