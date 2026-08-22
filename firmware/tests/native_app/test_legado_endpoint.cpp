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
  assert(M4LegadoBridge::baseUrlOk("http://reader.local:8080"));
  assert(!M4LegadoBridge::baseUrlOk("https://192.168.0.118:1122"));
  assert(!M4LegadoBridge::baseUrlOk("http://192.168.0.118:1122/path"));
  assert(!M4LegadoBridge::baseUrlOk("http://999.1.1.1:1122"));
  assert(M4LegadoBridge::makeBase("10.0.0.2", 4396) == "http://10.0.0.2:4396");
  assert(M4LegadoBridge::makeBase("bad host", 1122).empty());

  M4LegadoBridge::ParsedEndpoint parsed;
  std::string error;
  assert(M4LegadoBridge::parseEndpoint("192.168.1.20", "1122", parsed, &error));
  assert(parsed.base == "http://192.168.1.20:1122");
  assert(M4LegadoBridge::parseEndpoint(" http://reader.local:8080/ ", "1122", parsed, &error));
  assert(parsed.base == "http://reader.local:8080");
  assert(M4LegadoBridge::parseEndpoint("reader.local", "8081", parsed, &error));
  assert(parsed.base == "http://reader.local:8081");
  assert(!M4LegadoBridge::parseEndpoint("https://reader.local:8080", "1122", parsed, &error));
  assert(error == "unsupported_scheme");
  assert(!M4LegadoBridge::parseEndpoint("reader.local", "65536", parsed, &error));
  assert(error == "invalid_port");
  assert(!M4LegadoBridge::parseEndpoint("http://reader.local:8080/path", "1122", parsed, &error));
  assert(error == "unsupported_path");

  // A failed candidate must not replace the persisted/successful endpoint;
  // only the verified transition records it.
  M4LegadoBridge::ManualEndpointState endpointState;
  endpointState.lastSuccessful = "http://old.local:1122";
  endpointState.begin("http://new.local:8080");
  endpointState.fail("连接超时");
  assert(endpointState.phase == M4LegadoBridge::ManualEndpointPhase::Error);
  assert(endpointState.lastSuccessful == "http://old.local:1122");
  endpointState.begin("http://new.local:8080");
  endpointState.succeed();
  assert(endpointState.phase == M4LegadoBridge::ManualEndpointPhase::Ready);
  assert(endpointState.lastSuccessful == "http://new.local:8080");
  assert(M4LegadoBridge::endpointPath("/apps_data/com.legado.client") ==
         "/apps_data/com.legado.client/provider/endpoint.txt");

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
