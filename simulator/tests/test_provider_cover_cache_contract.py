import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CACHE = ROOT / "firmware/src/util/M4ProviderCoverCache.cpp"
HEADER = ROOT / "firmware/src/util/M4ProviderCoverCache.h"


class ProviderCoverCacheContracts(unittest.TestCase):
    def test_png_uses_existing_bounded_decoder_and_bmp_output_contract(self):
        source = CACHE.read_text(encoding="utf-8")
        header = HEADER.read_text(encoding="utf-8")
        self.assertIn("#include <PNGdec.h>", source)
        self.assertIn("pngFileToBmpStream", source)
        self.assertIn("decoder->getLineAsRGB565", source)
        self.assertIn("PNG_CHECK_CRC", source)
        self.assertIn("M4Psram::makeUnique<PNG>()", source)
        self.assertIn("M4Psram::mallocPrefer", source)
        self.assertIn("ImageFormat::Png", source)
        self.assertIn("format == ImageFormat::Png", header)
        self.assertIn("kMaxDownloadBytes", header)
        self.assertIn("if (format == ImageFormat::Png)", source)

    def test_unknown_and_webp_are_not_converted(self):
        header = HEADER.read_text(encoding="utf-8")
        self.assertIn("format == ImageFormat::Bmp", header)
        self.assertIn("format == ImageFormat::Jpeg", header)
        self.assertIn("format == ImageFormat::Png", header)
        self.assertIn("return format == ImageFormat::Bmp", header)
        self.assertIn("ImageFormat::Webp", header)


if __name__ == "__main__":
    unittest.main()
