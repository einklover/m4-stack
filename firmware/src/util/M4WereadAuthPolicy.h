#pragma once

#include <string>

namespace M4WereadAuthPolicy {

inline bool responseIndicatesLoginRequired(const std::string& prefix) {
  return prefix.find("-2012") != std::string::npos ||
         prefix.find("LOGIN_TIMEOUT") != std::string::npos ||
         prefix.find("login_required") != std::string::npos;
}

}  // namespace M4WereadAuthPolicy
