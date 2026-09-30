from pathlib import Path
r=Path(__file__).resolve().parents[2]
s=(r/"firmware/lib/EpdFontLoader/EpdFontLoader.cpp").read_text()
h=(r/"firmware/src/managers/FontManager.h").read_text()
m=s.split("const bool runtimeTtf =",1)[1].split("// Non-M4:",1)[0]
assert "releaseRuntimeTtfFaces(FontManager::TtfFaceRole::Reader)" in m
assert "clearLoadedReaderFonts()" in m
assert "releaseRuntimeTtfFaces();" not in m
assert "clearLoadedFonts();" not in m
assert "void clearLoadedReaderFonts();" in h
print("Reader and Chrome ownership: PASS")
