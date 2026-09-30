from pathlib import Path
r=Path(__file__).resolve().parents[2]
e=(r/"firmware/src/activities/reader/EpubReaderActivity.cpp").read_text()
s=(r/"firmware/src/activities/reader/EpubReaderSettingsActivity.cpp").read_text()
c=(r/"firmware/lib/EpdFont/TtfGlyphSdCache.cpp").read_text()
back=e.split("void EpubReaderActivity::onReaderMenuBack",1)[1].split("// Translate an absolute percent",1)[0]
assert "loadFontsFromSd" not in back
assert "pendingMenuClose_ = true" in back
assert "if (replaced && !subActivity && pendingMenuClose_.load())" in e
assert "if (subActivity || pendingMenuClose_.load())" in e
assert "EpdFontLoader::loadFontsFromSd(renderer);" not in s.split("void EpubReaderSettingsActivity::loop()",1)[1].split("const int count",1)[0]
assert c.count("ScopedSdCacheLock lock;") == 3
assert c.count("#if defined(ESP32) && M4_SD_GLYPH_CACHE_ENABLED") == 5
print("EPUB reload quiescence and SD cache serialization: PASS")
