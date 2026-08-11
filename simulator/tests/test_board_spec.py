import json
from pathlib import Path
import unittest

from tools import validate_board_spec


ROOT = Path(__file__).resolve().parents[1]
SPEC = ROOT / "board" / "murphy_m4.json"


class BoardSpecTests(unittest.TestCase):
    def load(self):
        return json.loads(SPEC.read_text(encoding="utf-8"))

    def test_current_spec_validates(self):
        self.assertEqual(validate_board_spec.validate(self.load()), [])

    def test_gpio_alias_is_rejected(self):
        spec = self.load()
        spec["frontlight"]["pins"]["cool"] = spec["display"]["pins"]["cs"]
        errors = validate_board_spec.validate(spec)
        self.assertTrue(any("conflicting primary owners" in e for e in errors))

    def test_schematic_flash_discrepancy_cannot_be_silently_erased(self):
        spec = self.load()
        spec["flash"]["status"] = "verified"
        errors = validate_board_spec.validate(spec)
        self.assertTrue(any("discrepancy" in e for e in errors))

    def test_physical_sensor_can_exist_while_firmware_disabled(self):
        spec = self.load()
        aht = next(d for d in spec["physical_i2c_devices"] if d["name"] == "AHT20")
        self.assertTrue(aht["schematic_present"])
        self.assertFalse(aht["firmware_enabled"])
        self.assertEqual(aht["address"], 0x38)


if __name__ == "__main__":
    unittest.main()
