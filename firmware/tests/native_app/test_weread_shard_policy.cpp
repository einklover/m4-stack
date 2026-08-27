#include "util/M4WereadAuthPolicy.h"
#include <cassert>
#include <cstring>

int main() {
  using M4WereadAuthPolicy::shardPrefixError;
  assert(shardPrefixError(R"({"bookId":"CB_imported"})", true) == nullptr);
  assert(std::strcmp(shardPrefixError(R"({"bookId":"CB_imported"})", false), "shard_json") == 0);
  assert(shardPrefixError("0123456789ABCDEF0123456789ABCDEFpayload", false) == nullptr);
  assert(std::strcmp(shardPrefixError("not-a-valid-md5-header-xxxxxxxxxxxxpayload", false), "shard_bad_header") == 0);
  assert(std::strcmp(shardPrefixError(R"({"errCode":-2012})", true), "login_required") == 0);
  return 0;
}
