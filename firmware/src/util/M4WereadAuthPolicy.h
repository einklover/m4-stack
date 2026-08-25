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

}  // namespace M4WereadAuthPolicy
