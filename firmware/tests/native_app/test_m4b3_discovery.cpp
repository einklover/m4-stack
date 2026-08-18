// Host tests for Browser Bridge mDNS/DNS-SD record contract.
// Build: g++-14 -std=c++14 -Wall -Wextra -Werror -I firmware/src
//        firmware/tests/native_app/test_m4b3_discovery.cpp

#include <cassert>
#include <cstdio>
#include <cstring>

#include "util/M4B3Discovery.h"

namespace {

using M4B3Discovery::Record;

void testConstants() {
  assert(std::strcmp(M4B3Discovery::kServiceType, "_m4b3._tcp") == 0);
  assert(std::strcmp(M4B3Discovery::kServiceTypeDot, "_m4b3._tcp.") == 0);
  assert(std::strcmp(M4B3Discovery::kInstanceName, "murphy-m4-browser") == 0);
  assert(std::strcmp(M4B3Discovery::kHostname, "murphy-m4") == 0);
  assert(M4B3Discovery::kPort == 48624);
  assert(std::strcmp(M4B3Discovery::kTxtProtoVal, "m4b3") == 0);
  assert(std::strcmp(M4B3Discovery::kTxtRoleVal, "browser-bridge") == 0);
}

void testParseValid() {
  Record r;
  assert(M4B3Discovery::parseRecord("murphy-m4-browser", "_m4b3._tcp.", "192.168.0.152", 48624, "m4b3", r));
  assert(r.ok);
  assert(std::strcmp(r.host, "192.168.0.152") == 0);
  assert(r.port == 48624);
  assert(std::strcmp(r.type, "_m4b3._tcp") == 0);
  assert(M4B3Discovery::parseRecord("  murphy-m4-browser  ", "_M4B3._TCP", "murphy-m4.local", 48624, "", r));
  assert(r.ok);
  assert(std::strcmp(r.host, "murphy-m4.local") == 0);
}

void testRejectInvalid() {
  Record r;
  assert(!M4B3Discovery::parseRecord("x", "_http._tcp", "192.168.0.152", 48624, "m4b3", r));
  assert(!M4B3Discovery::parseRecord("x", "_m4b3._tcp", "", 48624, "m4b3", r));
  assert(!M4B3Discovery::parseRecord("x", "_m4b3._tcp", "   ", 48624, "m4b3", r));
  assert(!M4B3Discovery::parseRecord("x", "_m4b3._tcp", "0.0.0.0", 48624, "m4b3", r));
  assert(!M4B3Discovery::parseRecord("x", "_m4b3._tcp", "255.255.255.255", 48624, "m4b3", r));
  assert(!M4B3Discovery::parseRecord("x", "_m4b3._tcp", "http://192.168.0.1", 48624, "m4b3", r));
  assert(!M4B3Discovery::parseRecord("x", "_m4b3._tcp", "192.168.0.1:48624", 48624, "m4b3", r));
  assert(!M4B3Discovery::parseRecord("x", "_m4b3._tcp", "fe80::1", 48624, "m4b3", r));
  assert(!M4B3Discovery::parseRecord("x", "_m4b3._tcp", "192.168.0.152", 0, "m4b3", r));
  assert(!M4B3Discovery::parseRecord("x", "_m4b3._tcp", "192.168.0.152", 65536, "m4b3", r));
  assert(!M4B3Discovery::parseRecord("x", "_m4b3._tcp", "192.168.0.152", -1, "m4b3", r));
  assert(!M4B3Discovery::parseRecord("x", "_m4b3._tcp", "192.168.0.152", 48624, "http", r));
  assert(M4B3Discovery::parseRecord("", "_m4b3._tcp", "192.168.0.152", 48624, "m4b3", r));
  assert(std::strcmp(r.name, M4B3Discovery::kInstanceName) == 0);
  assert(!M4B3Discovery::validHost("loopback"));
  assert(!M4B3Discovery::validHost("local"));
  assert(!M4B3Discovery::validPort(0));
  assert(M4B3Discovery::validPort(1));
  assert(M4B3Discovery::validPort(65535));
}

void testAdvertiseGate() {
  assert(!M4B3Discovery::advertiseAllowed(false, true, "192.168.0.152"));
  assert(!M4B3Discovery::advertiseAllowed(true, false, "192.168.0.152"));
  assert(!M4B3Discovery::advertiseAllowed(true, true, ""));
  assert(!M4B3Discovery::advertiseAllowed(true, true, "0.0.0.0"));
  assert(M4B3Discovery::advertiseAllowed(true, true, "192.168.0.152"));
}

void testPrecedenceRanks() {
  assert(M4B3Discovery::sourceRank(M4B3Discovery::Source::Manual) <
         M4B3Discovery::sourceRank(M4B3Discovery::Source::Discovered));
  assert(M4B3Discovery::sourceRank(M4B3Discovery::Source::Discovered) <
         M4B3Discovery::sourceRank(M4B3Discovery::Source::Cached));
  assert(M4B3Discovery::sourceRank(M4B3Discovery::Source::Cached) <
         M4B3Discovery::sourceRank(M4B3Discovery::Source::Loopback));
  assert(M4B3Discovery::sourceRank(M4B3Discovery::Source::Loopback) <
         M4B3Discovery::sourceRank(M4B3Discovery::Source::None));
}

}  // namespace

int main() {
  testConstants();
  testParseValid();
  testRejectInvalid();
  testAdvertiseGate();
  testPrecedenceRanks();
  std::printf("test_m4b3_discovery: PASS\n");
  return 0;
}
