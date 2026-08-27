#pragma once

#include <WiFi.h>

// wl_status_t collapses all station failures into WL_CONNECT_FAILED. The
// event payload retains the reason, so only explicit authentication/handshake
// failures are treated as a bad password. Other failures remain harmless.
class M4WifiFailureTracker final {
 public:
  M4WifiFailureTracker() {
    eventId_ = WiFi.onEvent(
        [this](arduino_event_id_t, arduino_event_info_t info) {
          const uint8_t reason = info.wifi_sta_disconnected.reason;
          if (reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
              reason == WIFI_REASON_HANDSHAKE_TIMEOUT || reason == WIFI_REASON_802_1X_AUTH_FAILED) {
            authFailure_ = true;
          }
        },
        ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  }

  ~M4WifiFailureTracker() {
    if (eventId_) WiFi.removeEvent(eventId_);
  }

  void reset() { authFailure_ = false; }
  bool authenticationFailed() const { return authFailure_; }

 private:
  wifi_event_id_t eventId_ = 0;
  volatile bool authFailure_ = false;
};
