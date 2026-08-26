#include "apps/M4xNetPolicy.h"
#include "util/M4WereadAuthPolicy.h"
#include <cassert>
#include <cstring>
#include <iostream>

int main() {
  using namespace M4xNetPolicy;
  assert(std::strcmp(M4WereadAuthPolicy::kRenewalPath, "/web/login/renewal") == 0);
  assert(std::strstr(M4WereadAuthPolicy::kRenewalBody, "ql") != nullptr);
  assert(M4WereadAuthPolicy::httpStatusMeansLoginRequired("http_401"));
  assert(M4WereadAuthPolicy::httpStatusMeansLoginRequired("http_403"));
  assert(!M4WereadAuthPolicy::httpStatusMeansLoginRequired("http_ESP_ERR_HTTP_CONNECT"));
  assert(M4WereadAuthPolicy::responseIndicatesLoginRequired("{\"errCode\":-2012}"));
  auto ql = parseWereadCookieAttrs("wr_ql=abc123; Path=/; HttpOnly");
  assert(ql.size() == 1);
  assert(ql[0].first == "wr_ql");
  assert(ql[0].second == "abc123");

  auto vid = parseWereadCookieAttrs("wr_vid=42; Path=/");
  assert(vid.size() == 1);
  assert(vid[0].first == "wr_vid");

  auto unrelated = parseWereadCookieAttrs("sessionid=nope; Path=/");
  assert(unrelated.empty());

  std::cout << "weread cookie policy OK\n";
  return 0;
}
