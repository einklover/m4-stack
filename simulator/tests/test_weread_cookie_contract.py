from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class WereadCookieContract(unittest.TestCase):
    def test_lua_preserves_all_wr_session_cookies(self):
        auth = (ROOT / "plugins/m4-weread-plugin/auth.lua").read_text(encoding="utf-8")
        self.assertIn('name:sub(1, 3) == "wr_"', auth)
        self.assertIn('for name, value in pairs(c)', auth)

    def test_native_replay_uses_shared_weread_cookie_policy(self):
        src = (ROOT / "firmware/src/apps/providers/M4NativeProviderIo.cpp").read_text(encoding="utf-8")
        self.assertIn("M4xNetPolicy::isWereadCookieName", src)

    def test_native_renews_before_qr_on_expired_shelf(self):
        login = (ROOT / "firmware/src/apps/providers/M4NativeProviderLogin.cpp").read_text(
            encoding="utf-8")
        discovery = (ROOT / "firmware/src/apps/providers/M4NativeProviderDiscovery.cpp").read_text(
            encoding="utf-8")
        lua = (ROOT / "plugins/m4-weread-plugin/auth.lua").read_text(encoding="utf-8")
        self.assertIn("kRenewalPath", login)
        self.assertIn("tryRenewSession", discovery)
        self.assertIn("/web/login/renewal", lua)


if __name__ == "__main__":
    unittest.main()
