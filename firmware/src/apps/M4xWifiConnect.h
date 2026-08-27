#pragma once

// Host-testable Wi-Fi connection lifecycle for M4x apps with network permission.
//
// Ownership rules:
//  - net.isConnected() always reports live radio state (never lies).
//  - net.connectSaved tries saved credentials deterministically.
//  - The runtime claims ownership only when *it* establishes a new connection.
//  - On app exit, disconnect only if the runtime owned the connection.
//  - Never call disconnect(true) (erase credentials is forbidden).
//  - Passwords must never appear in logs or error strings.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace M4xWifiConnect {

inline constexpr int kDefaultTimeoutMs = 20000;
inline constexpr int kMinTimeoutMs = 1000;
inline constexpr int kMaxTimeoutMs = 60000;
// Per-credential attempt budget within the overall deadline.
inline constexpr int kPerCredentialMs = 8000;
inline constexpr int kPollIntervalMs = 100;

inline int clampTimeoutMs(int ms) {
  if (ms < kMinTimeoutMs) return kMinTimeoutMs;
  if (ms > kMaxTimeoutMs) return kMaxTimeoutMs;
  return ms;
}

// Stable Lua/API error codes (never contain secrets).
inline constexpr const char* kErrNone = "";
inline constexpr const char* kErrNoSavedWifi = "no_saved_wifi";
inline constexpr const char* kErrTimeout = "timeout";
inline constexpr const char* kErrCancelled = "cancelled";
inline constexpr const char* kErrConnectFailed = "connect_failed";
inline constexpr const char* kErrAuthFailed = "auth_failed";
inline constexpr const char* kErrAuthStoreFailed = "auth_credential_not_cleared";
inline constexpr const char* kErrAlreadyConnected = "";  // success path

enum class RadioStatus : uint8_t {
  Idle = 0,
  Connecting,
  Connected,
  Failed,      // generic failure; credentials are preserved
  AuthFailed,  // platform explicitly reported authentication failure
};

// Abstract radio for production WiFi and host-test mocks.
struct IRadio {
  virtual ~IRadio() = default;
  virtual bool isConnected() const = 0;
  virtual std::string connectedSsid() const = 0;
  virtual RadioStatus status() const = 0;
  // Begin STA association. Implementations must not log password.
  virtual void begin(const std::string& ssid, const std::string& password) = 0;
  // disconnect(false): keep credentials; do not erase NVS/store.
  virtual void disconnectKeepCreds() = 0;
  virtual void setStaMode() = 0;
};

struct Credential {
  std::string ssid;
  std::string password;
};

struct ConnectResult {
  bool ok = false;
  // True only when this session established a new link (caller must release on exit).
  bool owned = false;
  bool credentialInvalidationFailed = false;
  std::string ssid;
  std::string error;  // stable code; empty on success
};

// Clock / cancel / sleep hooks (host tests inject fakes).
struct Hooks {
  std::function<uint32_t()> nowMs;           // required
  std::function<void(uint32_t)> sleepMs;     // required
  std::function<bool()> isCancelled;         // optional; null => never
  // Called only for an explicit platform authentication failure. Returning
  // false means the credential could not be invalidated durably.
  std::function<bool(const std::string&)> onAuthFailure;
};

// Try credentials in order until connected, deadline, cancel, or exhaustion.
// If already connected on entry: ok=true, owned=false (do not take ownership).
inline ConnectResult connectSaved(IRadio& radio, const std::vector<Credential>& creds, int timeoutMs,
                                  const Hooks& hooks) {
  ConnectResult r;
  if (!hooks.nowMs || !hooks.sleepMs) {
    r.error = kErrConnectFailed;
    return r;
  }
  timeoutMs = clampTimeoutMs(timeoutMs);
  const uint32_t deadline = hooks.nowMs() + static_cast<uint32_t>(timeoutMs);

  auto cancelled = [&]() -> bool { return hooks.isCancelled && hooks.isCancelled(); };
  bool sawAuthFailure = false;
  bool authInvalidationFailed = false;

  if (radio.isConnected()) {
    r.ok = true;
    r.owned = false;
    r.ssid = radio.connectedSsid();
    r.error = kErrNone;
    return r;
  }

  if (creds.empty()) {
    r.error = kErrNoSavedWifi;
    return r;
  }

  for (size_t i = 0; i < creds.size(); ++i) {
    if (cancelled()) {
      r.error = kErrCancelled;
      radio.disconnectKeepCreds();
      return r;
    }
    const uint32_t now = hooks.nowMs();
    if (now >= deadline) {
      r.error = kErrTimeout;
      radio.disconnectKeepCreds();
      return r;
    }

    const uint32_t remaining = deadline - now;
    const uint32_t attemptBudget =
        remaining < static_cast<uint32_t>(kPerCredentialMs) ? remaining : static_cast<uint32_t>(kPerCredentialMs);
    const uint32_t attemptDeadline = now + attemptBudget;

    radio.setStaMode();
    // Never log password.
    radio.begin(creds[i].ssid, creds[i].password);

    while (hooks.nowMs() < attemptDeadline) {
      if (cancelled()) {
        r.error = kErrCancelled;
        radio.disconnectKeepCreds();
        return r;
      }
      if (radio.isConnected()) {
        r.ok = true;
        r.owned = true;
        r.ssid = radio.connectedSsid();
        if (r.ssid.empty()) r.ssid = creds[i].ssid;
        r.error = kErrNone;
        return r;
      }
      const RadioStatus st = radio.status();
      if (st == RadioStatus::AuthFailed) {
        sawAuthFailure = true;
        if (hooks.onAuthFailure && !hooks.onAuthFailure(creds[i].ssid)) authInvalidationFailed = true;
        break;
      }
      if (st == RadioStatus::Failed) {
        break;  // try next credential
      }
      hooks.sleepMs(static_cast<uint32_t>(kPollIntervalMs));
    }

    // Soft timeout or fail for this SSID — disconnect before next attempt.
    radio.disconnectKeepCreds();
  }

  if (cancelled()) {
    r.error = kErrCancelled;
  } else if (hooks.nowMs() >= deadline) {
    r.error = kErrTimeout;
  } else {
    // All credentials exhausted before global deadline.
    if (sawAuthFailure) {
      r.error = authInvalidationFailed ? kErrAuthStoreFailed : kErrAuthFailed;
      r.credentialInvalidationFailed = authInvalidationFailed;
    } else {
      r.error = kErrConnectFailed;
    }
  }
  return r;
}

// Release radio only when the runtime owned the session connection.
inline void releaseIfOwned(IRadio& radio, bool& owned) {
  if (!owned) return;
  radio.disconnectKeepCreds();
  owned = false;
}

// Map stable error codes to short user-facing English (ASCII-safe for subset font).
inline const char* userHintAscii(const std::string& err) {
  if (err == kErrNoSavedWifi) return "No saved Wi-Fi. Connect in Settings.";
  if (err == kErrTimeout) return "Wi-Fi timeout. Retry or check Settings.";
  if (err == kErrCancelled) return "Wi-Fi connect cancelled.";
  if (err == kErrAuthFailed) return "Wi-Fi password rejected. Re-enter it in Settings.";
  if (err == kErrAuthStoreFailed) return "Wi-Fi password rejected; saved password was kept.";
  if (err == kErrConnectFailed) return "Saved Wi-Fi failed. Retry or Settings.";
  if (err.empty()) return "OK";
  return "Network error. Retry.";
}

}  // namespace M4xWifiConnect
