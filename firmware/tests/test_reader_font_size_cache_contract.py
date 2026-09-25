#!/usr/bin/env python3
"""Guard persistence of the actual pixel size for M4 Reader caches."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
epub = (root / "lib/Epub/Epub/Section.cpp").read_text()
txt = (root / "src/activities/reader/TxtReaderActivity.cpp").read_text()
for name, code, v in [("EPUB", epub, "SECTION_FILE_VERSION = 16"),
                       ("TXT", txt, "CACHE_VERSION = 11")]:
    assert v in code, name
    assert "readerPx" in code if name == "EPUB" else "SETTINGS.getReaderPixelSize()" in code
# One EPUB header write, one EPUB header read and comparison.
assert epub.count("serialization::writePod(file, readerPx);") == 1
assert epub.count("serialization::readPod(file, fileReaderPx);") == 1
assert "fileReaderPx != readerPx" in epub
# Prefetch and normal TXT cache writers MUST agree with the cache reader.
assert txt.count("serialization::writePod(f, SETTINGS.getReaderPixelSize());") == 2
assert txt.count("serialization::readPod(f, cachedReaderPx);") == 1
assert "cachedReaderPx != SETTINGS.getReaderPixelSize()" in txt
print("Reader font-size persisted cache contracts: PASS")
