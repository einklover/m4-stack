import unittest

from tools import import_device_trace


class DeviceTraceTests(unittest.TestCase):
    def test_imports_known_murphy_diagnostics(self):
        parsed = import_device_trace.parse_lines([
            "heap free_heap=70840 min_free_heap=68888 free_psram=7605792 largest_internal=31984\n",
            "[50093] open path=cache/6259312/ch_1.txt load_ms=16 ok=1 size=12967 free=139976\n",
            "[PTSH] t=701 DIFF event=commit mask=0x08 legacy{t=2} shadow{t=2}\n",
            "panic reason=Unknown core=0 pc=0x400559e0 frames=27 provider_stage=0x00000310\n",
            "Waveform TP=0x30 duration=1061 ms\n",
        ])
        self.assertEqual(parsed["heap_samples"][0]["free_heap"], 70840)
        self.assertEqual(parsed["reader_opens"][0]["path"], "cache/6259312/ch_1.txt")
        self.assertEqual(parsed["page_turn_diffs"][0]["mask"], 8)
        self.assertEqual(parsed["panics"][0]["provider_stage"], 0x310)
        self.assertEqual(parsed["epd_timings"][0]["duration_ms"], 1061)
        self.assertEqual(parsed["raw_unclassified"], [])


if __name__ == "__main__":
    unittest.main()
