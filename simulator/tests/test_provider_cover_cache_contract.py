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

    def test_home_scene_sizes_generate_from_source_img_without_fetch(self):
        header = HEADER.read_text(encoding="utf-8")
        source = CACHE.read_text(encoding="utf-8")
        home = (ROOT / "firmware/src/activities/home/HomeActivity.cpp").read_text(encoding="utf-8")
        self.assertIn("ensureSizedCoverFromSource", header)
        self.assertIn("source.img", header)
        self.assertIn("Never fetches", header)
        self.assertIn("jpegFileTo1BitBmpStreamWithSize", source)
        self.assertIn("convertCoverFile(source, target, w, h, true)", source)
        self.assertNotIn("backend.fetch", source[source.find("bool ensureSizedCoverFromSource"):])
        self.assertIn("M4ProviderCoverCache::ensureSizedCoverFromSource", home)
        self.assertIn("kHomeCurrentCoverW", home)
        self.assertIn("kHomeRecentCoverW", home)

    # ---- Round-1 strengthened generate-on-miss contracts ----

    def test_ensure_sized_cache_hit_when_target_exists(self):
        header = HEADER.read_text(encoding="utf-8")
        # Inline ensureSized must check target existence first and return cacheHit
        self.assertIn("const std::string target = sizedBmpPathInDir(dir, width, height);", header)
        self.assertIn("if (backend.exists(target))", header)
        self.assertIn("out.cacheHit = true;", header)
        # Hit must set thumbPath to target
        self.assertIn("out.thumbPath = target;", header)
        # And return early without converting
        hit_section = header[header.find("if (backend.exists(target))"):header.find("if (cancelled && cancelled())")]
        self.assertIn("return out;", hit_section)

    def test_ensure_sized_generates_1bit_exact_from_source_img(self):
        header = HEADER.read_text(encoding="utf-8")
        source = CACHE.read_text(encoding="utf-8")
        # Header must derive source as source.img in same dir, and target as cover_{w}x{h}.bmp
        self.assertIn("sourcePathInDir(dir)", header)
        self.assertIn("sizedBmpPathInDir(dir, width, height)", header)
        # Conversion must be called with exact WxH and must exist check afterwards
        self.assertIn("backend.convert(source, target, width, height)", header)
        self.assertIn("!backend.exists(target)", header)
        # Production oneBit path must use 1-bit converter
        self.assertIn("convertCoverFile(source, target, w, h, true)", source)
        self.assertIn("jpegFileTo1BitBmpStreamWithSize", source)
        # 1-bit BMP header uses 1 bpp, palette 2 entries, 62 byte offset — check converter helpers exist
        # The production converter explicitly writes 1-bit header
        jconv = (ROOT / "firmware/lib/JpegToBmpConverter/JpegToBmpConverter.cpp").read_text(encoding="utf-8")
        self.assertIn("writeBmpHeader1bit", jconv)
        self.assertIn("Bits per pixel (1 bit)", jconv)
        # Exact-size aspect-fill path is required for Home Scene thumbs (110x180 / 74x106)
        self.assertIn("exactTarget", jconv)
        self.assertIn("Exact-size aspect-fill", jconv)

    def test_ensure_sized_never_http_never_fetch(self):
        header = HEADER.read_text(encoding="utf-8")
        source = CACHE.read_text(encoding="utf-8")
        # Inline ensureSized must never fetch/HTTP; it only converts from source.img
        inline_slice = header[header.find("inline EnsureSizedResult ensureSizedCoverFromSource"):header.find("inline Result acquire")]
        self.assertNotIn("backend.fetch", inline_slice)
        self.assertNotIn("M4NativeProviderHttp", inline_slice)
        self.assertNotIn("http", inline_slice.lower())
        # Production wrapper also must not fetch inside ensureSized
        prod_slice = source[source.find("bool ensureSizedCoverFromSource"):]
        self.assertNotIn("backend.fetch", prod_slice)
        self.assertNotIn("M4NativeProviderHttp::requestToSink", prod_slice)
        self.assertNotIn("fetchCancellable", prod_slice)
        # Must still contain "Never fetches" comment
        self.assertIn("Never fetches", header)

    def test_ensure_sized_missing_source_does_not_invent_download(self):
        header = HEADER.read_text(encoding="utf-8")
        # When source.img missing, must return empty thumbPath without fetching.
        # Header uses `if (backend.exists(source)) { ... }` positive branch for generate,
        # and the else branch (last resort) handles missing source without fetch.
        # Check that missing-source path does not fetch and clears thumb for probes.
        ensure_slice = header[header.find("inline EnsureSizedResult ensureSizedCoverFromSource"):header.find("inline Result acquire")]
        self.assertIn("backend.exists(source)", ensure_slice)
        # The generate branch is guarded by exists(source); missing source must not call fetch
        self.assertNotIn("backend.fetch", ensure_slice)
        self.assertIn("out.thumbPath.clear();", ensure_slice)

    def test_ensure_sized_convert_failure_removes_partial_target(self):
        header = HEADER.read_text(encoding="utf-8")
        # On convert failure or missing target after convert, must remove partial file
        self.assertIn("if (!backend.convert(source, target, width, height) || !backend.exists(target))", header)
        fail_slice = header[header.find("if (!backend.convert(source, target, width, height)"):header.find("if (cancelled && cancelled())", header.find("if (!backend.convert(source, target, width, height)"))]
        self.assertIn("if (backend.remove) backend.remove(target);", fail_slice)
        self.assertIn("out.thumbPath.clear();", fail_slice)

    def test_ensure_sized_path_must_be_under_provider_covers(self):
        header = HEADER.read_text(encoding="utf-8")
        # Must validate that coverBmpPath is under /.crosspoint/provider_covers/
        self.assertIn("directoryOfCoverPath(coverBmpPath)", header)
        self.assertIn("isProviderCoverCacheDir(dir)", header)
        self.assertIn("dir.empty() || !isProviderCoverCacheDir(dir)", header)
        self.assertIn("/.crosspoint/provider_covers/", header)
        # Helpers must exist
        self.assertIn("inline bool isProviderCoverCacheDir", header)
        self.assertIn("inline std::string directoryOfCoverPath", header)
        self.assertIn("inline std::string sourcePathInDir", header)
        self.assertIn("inline std::string sizedBmpPathInDir", header)
        # acquire path helpers also scoped
        self.assertIn("inline std::string cacheDir", header)
        self.assertIn("inline std::string bmpTemplatePath", header)
        self.assertIn("inline std::string sourcePath", header)
        self.assertIn("inline std::string concreteBmpPath", header)

    def test_ensure_sized_cancelled_semantics(self):
        header = HEADER.read_text(encoding="utf-8")
        # Must check cancelled before convert (no work if cancelled)
        self.assertIn("if (cancelled && cancelled())", header)
        # There are two cancelled checks: one before source exists, one after convert (keep file for next paint)
        count = header.count("if (cancelled && cancelled())")
        self.assertGreaterEqual(count, 2, "expected pre- and post-convert cancelled checks")
        # Post-convert cancelled still keeps file (comment says keep file so next paint is hit)
        self.assertIn("Conversion finished; keep the file so the next Home paint is a hit", header)

    def test_last_resort_from_171x254_is_guarded_or_absent(self):
        header = HEADER.read_text(encoding="utf-8")
        source = CACHE.read_text(encoding="utf-8")
        has_171 = "cover_171x254" in header or "cover_171x254" in source
        if not has_171:
            self.skipTest("last-resort cover_171x254 API not in this worktree yet (Lane A) — guard passes")
        else:
            combined = header + source
            self.assertIn("source.img", combined)
            self.assertIn("Never fetches", header)
            self.assertIn("fallbackBmpPathInDir", header)
            self.assertIn("bmpFileTo1BitBmpWithSize", source)
            # Ensure last-resort occurs after source block: search within ensure function
            ensure_slice = header[header.find("inline EnsureSizedResult ensureSizedCoverFromSource"):header.find("inline Result acquire")]
            idx_src = ensure_slice.find("if (backend.exists(source))")
            idx_fb = ensure_slice.find("fallbackBmpPathInDir")
            self.assertNotEqual(idx_src, -1)
            self.assertNotEqual(idx_fb, -1)
            self.assertGreater(idx_fb, idx_src, "last-resort must run only after source.img exists-check")


if __name__ == "__main__":
    unittest.main()
