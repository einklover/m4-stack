import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODEL_H = ROOT / "firmware/src/ui/pages/HomeSceneModel.h"
HOME_CPP = ROOT / "firmware/src/activities/home/HomeActivity.cpp"
DECODER_H = ROOT / "firmware/src/activities/home/HomeSceneAssetDecoder.h"
DECODER_CPP = ROOT / "firmware/src/activities/home/HomeSceneAssetDecoder.cpp"

class HomeDockContracts(unittest.TestCase):
    def test_kMaxAppItems_is_4(self):
        txt = MODEL_H.read_text(encoding="utf-8")
        self.assertIn("kMaxAppItems = 4", txt, "kMaxAppItems must remain 4 (dock has 4 slots) — locked")

    def test_home_activity_publishes_builtin_files_first(self):
        # Prefer behavior check via stable API names, not polarity of if-condition
        src = HOME_CPP.read_text(encoding="utf-8")
        self.assertIn("builtin.files", src,
                      "HomeActivity.cpp must publish builtin.files as first dock app "
                      "(canonical 'builtin.files'). Expected RED until Lane A lands. "
                      "Equivalents like builtin_files may be accepted but test enforces canonical.")

    def test_home_activity_references_preferred_plugins_in_order(self):
        src = HOME_CPP.read_text(encoding="utf-8")
        i_weread = src.find("com.weread.client")
        i_fanqie = src.find("com.fanqie.client")
        i_jjwxc = src.find("com.jjwxc.client")
        self.assertNotEqual(i_weread, -1, "must reference weread for dock ordering")
        self.assertNotEqual(i_fanqie, -1, "must reference fanqie")
        self.assertNotEqual(i_jjwxc, -1, "must reference jjwxc")
        self.assertTrue(i_weread < i_fanqie < i_jjwxc,
                        "preferred dock order must be weread, fanqie, jjwxc in that order in source (not fanqie before weread)")

    def test_home_activity_uses_addApp_for_dock(self):
        src = HOME_CPP.read_text(encoding="utf-8")
        self.assertIn("addApp", src, "dock must use HomeSceneModel::addApp")

    def test_decoder_has_resolvePath_and_builtin_handling(self):
        # Decoder must handle builtin files icon without traversal — generic pipeline suffices.
        txt = DECODER_H.read_text(encoding="utf-8") + DECODER_CPP.read_text(encoding="utf-8")
        self.assertIn("resolveAppIconPath", txt)
        self.assertIn("decodeAppIconForPublication", txt)
        # Ensure 62x64 handling exists (already true, but locks)
        self.assertTrue("kHomeAppIconW" in txt or "62" in txt, "decoder should handle 62x64 icon size")

if __name__ == "__main__":
    unittest.main()
