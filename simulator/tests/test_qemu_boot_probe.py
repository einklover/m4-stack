import unittest

from qemu import probe_boot


class QemuProbeTests(unittest.TestCase):
    def test_classifies_board_init_then_sd_failure(self):
        log = """
ESP-ROM:esp32s3-20210327
I (20) boot: ESP-IDF second stage bootloader
I (44) boot: Partition Table:
I (210) app: Arduino setup() Murphy M4
I (230) spiram: Found 8MB PSRAM
I (260) BoardConfig: FreeInk SSD1677 init
E (410) SDMMC: mount fail
"""
        result = probe_boot.classify(log)
        self.assertEqual(result["highest_stage"], "board_init")
        self.assertEqual(result["failure_class"], "sdmmc")
        self.assertTrue(result["acceptance"]["application_reached"])
        self.assertTrue(result["acceptance"]["board_initialization_reached"])

    def test_fatal_exception_remains_failure(self):
        result = probe_boot.classify(
            "ESP-ROM\nbootloader\nArduino setup()\nGuru Meditation Error: LoadProhibited\n"
        )
        self.assertEqual(result["failure_class"], "panic")
        self.assertEqual(result["highest_stage"], "app")

    def test_real_qemu_uart_checkpoints_reach_setup(self):
        result = probe_boot.classify(
            "ESP-ROM:esp32s3-20210327\n"
            "entry 0x403c88b8\n"
            "[1192] [M4-RC1] setup() start ver=murphy-m4-qemu\n"
            "[1509] [M4-PSRAM] free=8384116 total=8388608\n"
        )
        self.assertEqual(result["highest_stage"], "psram")
        self.assertTrue(result["acceptance"]["second_stage_bootloader_reached"])
        self.assertTrue(result["acceptance"]["firmware_setup_started"])


if __name__ == "__main__":
    unittest.main()
