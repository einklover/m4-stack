"""Host checks for the Legado plugin package and native endpoint route."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import zipfile
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[2]
PLUGIN = ROOT / "plugins" / "m4-legado-plugin"


class LegadoPluginTests(unittest.TestCase):
    def test_manifest_and_entry_are_loadable(self) -> None:
        manifest = json.loads((PLUGIN / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["id"], "com.legado.client")
        self.assertEqual(manifest["runtime"], "native")
        self.assertEqual(manifest["provider"], "legado")
        self.assertEqual(manifest["entry"], "main.xml")

        root = ET.parse(PLUGIN / "main.xml").getroot()
        self.assertEqual(root.tag, "m4ui")
        buttons = root.find("./screen/buttons")
        self.assertIsNotNone(buttons)
        self.assertEqual(buttons.attrib.get("onLeft"), "provider.endpoint")

    def test_package_contains_native_entry(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            package = Path(temp_dir) / "legado.m4x"
            subprocess.run(
                [
                    "python3",
                    str(ROOT / "plugins" / "m4-screen-bridge-plugin" / "tools" / "package.py"),
                    str(PLUGIN),
                    str(package),
                ],
                check=True,
                cwd=ROOT,
                stdout=subprocess.PIPE,
                text=True,
            )
            with zipfile.ZipFile(package) as archive:
                self.assertEqual(set(archive.namelist()), {"manifest.json", "main.xml"})
                packaged_manifest = json.loads(archive.read("manifest.json"))
                self.assertEqual(packaged_manifest["provider"], "legado")
                self.assertIn("provider.endpoint", archive.read("main.xml").decode("utf-8"))

    def test_firmware_route_is_present(self) -> None:
        template = (ROOT / "firmware" / "src" / "apps" / "native" / "M4NativeProviderHomeTemplate.h").read_text(
            encoding="utf-8"
        )
        activity = (ROOT / "firmware" / "src" / "activities" / "apps" / "NativeAppActivity.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn('onLeft="provider.endpoint"', template)
        self.assertIn("ActionKind::OpenEndpoint", activity)


if __name__ == "__main__":
    unittest.main()
