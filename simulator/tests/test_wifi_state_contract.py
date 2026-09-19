"""Regression contracts for the M4 Wi-Fi credential/readiness redesign."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


class WifiStateContractTests(unittest.TestCase):
    def test_successful_manual_connection_prompts_before_saving(self) -> None:
        # Phase 2A Task 2 (approved behavior change): new credentials must not
        # auto-save. Success enters an explicit Yes/No prompt; only Yes
        # reaches WifiCredentialStore, No keeps the live session.
        activity = (ROOT / "firmware/src/activities/network/WifiSelectionActivity.cpp").read_text()
        check = activity[activity.index("void WifiSelectionActivity::checkConnectionStatus"):]
        check = check[:check.index("void WifiSelectionActivity::loop")]
        self.assertNotIn("WIFI_STORE.addCredential", check)
        self.assertIn("state = WifiSelectionState::CONNECTED", check)
        self.assertIn("m4WifiSaveConfirmed", activity)
        self.assertIn("WIFI_STORE.addCredential(selectedSSID, enteredPassword)", activity)
        self.assertIn("是否保存", activity)
        self.assertNotIn('connectionError = "Connected, but could not save Wi-Fi"', activity)

    def test_existing_crosspoint_dir_does_not_fail_wifi_save(self) -> None:
        store = (ROOT / "firmware/src/WifiCredentialStore.cpp").read_text()
        self.assertNotIn('if (!SdMan.mkdir("/.crosspoint")) return false;', store)
        self.assertIn('!SdMan.exists("/.crosspoint") && !SdMan.mkdir("/.crosspoint")', store)

    def test_provider_readiness_is_live_station_with_ip(self) -> None:
        native_wifi = (ROOT / "firmware/src/apps/providers/M4NativeWifi.cpp").read_text()
        transport = (ROOT / "firmware/src/apps/M4HttpTransport.cpp").read_text()
        lua_host = (ROOT / "firmware/src/apps/M4xLuaHost.cpp").read_text()
        self.assertIn("return M4QemuNet::staConnected();", native_wifi)
        self.assertIn('if (!M4NativeWifi::isReady())', transport)
        self.assertIn("WiFi.localIP() != IPAddress(0, 0, 0, 0)", lua_host)

    def test_only_platform_auth_reason_can_invalidate_saved_credentials(self) -> None:
        tracker = (ROOT / "firmware/src/apps/M4WifiFailureTracker.h").read_text()
        policy = (ROOT / "firmware/src/apps/M4xWifiConnect.h").read_text()
        self.assertIn("WIFI_REASON_AUTH_FAIL", tracker)
        self.assertIn("WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT", tracker)
        self.assertIn("RadioStatus::AuthFailed", policy)
        self.assertIn("hooks.onAuthFailure", policy)
        self.assertIn("RadioStatus::Failed", policy)


if __name__ == "__main__":
    unittest.main()
