"""Offline tests for the M4 PR evidence gate."""
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_m4_workflow as gate
SHA = "a" * 40

def body(risk="low", sha=SHA, muse="passed: actual independent review report"):
    return (f"- **Risk**: {risk}\n- **Frozen SHA**: {sha}\n"
            "- **Targeted tests**: passed: python3 firmware/tests/example.py exit 0\n"
            f"- **Muse review**: {muse}\n- **Astra**: not requested\n")

class GateTests(unittest.TestCase):
    def test_valid_low(self): self.assertFalse(gate.validate_evidence(body(), SHA, False))
    def test_high_requires_high(self):
        self.assertTrue(gate.validate_evidence(body(), SHA, True))
        self.assertFalse(gate.validate_evidence(body(risk="high"), SHA, True))
    def test_refreeze_after_commit(self):
        self.assertTrue(gate.validate_evidence(body(sha="b" * 40), SHA, False))
    def test_no_placeholder_muse(self):
        self.assertTrue(gate.validate_evidence(body(muse="pending"), SHA, False))
    def test_astra_no_auto(self):
        self.assertTrue(gate.validate_evidence(body().replace("not requested", "automatic"), SHA, False))
    def test_doc_only_and_risk_classification(self):
        self.assertEqual(gate.classify(["docs/readme.md", "AGENTS.md"]), (False, False, False))
        self.assertEqual(gate.classify(["firmware/src/main.cpp"]), (True, True, True))
        self.assertEqual(gate.classify(["plugins/example/manifest.json"]), (True, False, False))
    def test_reject_generated_artifacts(self):
        self.assertEqual(len(gate.forbidden(["firmware/.pio/build/firmware.bin", "plugins/app.m4x"])), 2)
        self.assertFalse(gate.forbidden(["firmware/src/components/icons/app_store_generated.h"]))

if __name__ == "__main__":
    unittest.main()
