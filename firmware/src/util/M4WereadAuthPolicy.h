#pragma once

#include <string>

namespace M4WereadAuthPolicy {

// Same path/body as plugins/m4-weread-plugin/auth.lua Auth.try_renew.
inline constexpr const char* kRenewalPath = "/web/login/renewal";
inline constexpr const char* kRenewalBody = "{\"rq\":\"%2Fweb%2Fbook%2Fread\",\"ql\":false}";

inline bool responseIndicatesLoginRequired(const std::string& prefix) {
  return prefix.find("-2012") != std::string::npos ||
         prefix.find("LOGIN_TIMEOUT") != std::string::npos ||
         prefix.find("login_required") != std::string::npos;
}

inline bool httpStatusMeansLoginRequired(const std::string& error) {
  return error == "http_401" || error == "http_403" || error == "login_required";
}

// Validate the lightweight framing contract used by WeRead chapter endpoints.
// e_0 is special for user-imported text books: it is legitimate routing JSON,
// while t_*/e_* content shards are framed as 32 ASCII hex MD5 bytes + payload.
inline const char* shardPrefixError(const std::string& prefix, bool allowJson) {
  if (responseIndicatesLoginRequired(prefix)) return "login_required";
  if (!prefix.empty() && prefix[0] == '{') {
    return allowJson ? nullptr : "shard_json";
  }
  if (prefix.size() >= 32) {
    for (size_t i = 0; i < 32; ++i) {
      const char c = prefix[i];
      const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                       (c >= 'A' && c <= 'F');
      if (!hex) return "shard_bad_header";
    }
  }
  return nullptr;
}

}  // namespace M4WereadAuthPolicy
