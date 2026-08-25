from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class NativeCatalogMemoryContract(unittest.TestCase):
    def test_catalog_task_uses_policy_stack_size(self):
        src = (ROOT / "firmware/src/apps/providers/M4NativeProviderCatalog.cpp").read_text(encoding="utf-8")
        self.assertIn("M4NativeCatalogPolicy::kTaskStackBytes", src)

    def test_full_catalog_path_uses_provider_memory_policy(self):
        src = (ROOT / "firmware/src/apps/providers/M4NativeProviderCatalog.cpp").read_text(encoding="utf-8")
        self.assertIn("M4NativeCatalogPolicy::preferPsramAssembly(job.providerId)", src)


if __name__ == "__main__":
    unittest.main()
