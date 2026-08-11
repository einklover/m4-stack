// Host regression for Legado auto-endpoint helpers (no SD / no network).
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "apps/providers/M4LegadoBridge.h"
#include "apps/providers/M4LanVisitorStore.h"

int main() {
  // --- visitor store parse/serialize ---
  std::vector<M4LanVisitorStore::SsidVisitors> rows;
  const char* raw =
      "HomeWifi\t192.168.0.118,192.168.0.50\n"
      "Office\t10.0.0.9\n"
      "# comment\n"
      "bad\tnot-an-ip\n";
  assert(M4LanVisitorStore::parseStore(raw, rows));
  assert(rows.size() == 2);
  assert(rows[0].ssid == "HomeWifi");
  assert(rows[0].ips.size() == 2);
  assert(rows[0].ips[0] == "192.168.0.118");
  assert(rows[1].ssid == "Office");

  const std::string round = M4LanVisitorStore::serializeStore(rows);
  assert(round.find("HomeWifi\t192.168.0.118,192.168.0.50") != std::string::npos);

  assert(M4LanVisitorStore::ipOk("192.168.1.1"));
  assert(!M4LanVisitorStore::ipOk("0.0.0.0"));
  assert(!M4LanVisitorStore::ipOk("127.0.0.1"));
  assert(!M4LanVisitorStore::ipOk("1.2.3"));
  assert(!M4LanVisitorStore::ipOk("abc"));

  // --- endpoint helpers ---
  assert(M4LegadoBridge::baseUrlOk("http://192.168.0.118:1122"));
  assert(!M4LegadoBridge::baseUrlOk("https://192.168.0.118:1122"));
  assert(!M4LegadoBridge::baseUrlOk("http://192.168.0.118:1122/path"));
  assert(!M4LegadoBridge::baseUrlOk("http://evil.com:1122"));
  assert(M4LegadoBridge::makeBase("10.0.0.2", 4396) == "http://10.0.0.2:4396");
  assert(M4LegadoBridge::makeBase("bad", 1122).empty());

  assert(M4LegadoBridge::probeBodyLooksLikeLegado(
      R"({"data":[],"isSuccess":true})", 200, ""));
  assert(M4LegadoBridge::probeBodyLooksLikeLegado(
      R"({"data":[{"bookUrl":"x","name":"a"}]})", 200, ""));
  assert(!M4LegadoBridge::probeBodyLooksLikeLegado("not json", 404, "http_404"));

  // Port table must include the official default first.
  assert(M4LegadoBridge::kProbePortCount >= 4);
  assert(M4LegadoBridge::kProbePorts[0] == 1122);

  printf("legado endpoint + lan visitor helpers: PASS\n");
  return 0;
}
