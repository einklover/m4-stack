#include "apps/M4xNetPolicy.h"
#include <cassert>
#include <iostream>

int main() {
  using namespace M4xNetPolicy;
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
