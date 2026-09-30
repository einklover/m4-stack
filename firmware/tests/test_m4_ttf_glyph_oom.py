"""Source contract: glyph bad_alloc is caught and the face lock is released."""
from pathlib import Path

from test_m4_astra_stability import function

CPP = Path(__file__).resolve().parents[1] / 'lib/EpdFont/TtfEpdFont.cpp'


def test_source():
    text = CPP.read_text()
    lock = function(text, 'class FaceLock {')
    assert '~FaceLock()' in lock and 'xSemaphoreGive' in lock
    wrap = function(text, 'auto m4UnderFaceLock(')
    assert 'FaceLock lock' in wrap
    assert wrap.index('FaceLock lock') < wrap.index('catch (const std::bad_alloc&)')
    for sig in (
        'const EpdGlyph* TtfEpdFont::getGlyph(',
        'const uint8_t* TtfEpdFont::loadGlyphBitmap(',
        'int TtfEpdFont::glyphAdvanceX(',
        'void TtfEpdFont::clearCaches(',
    ):
        body = function(text, sig)
        assert 'm4UnderFaceLock' in body
        assert 'xSemaphoreTake' not in body and 'xSemaphoreGive' not in body
    print('ttf glyph oom source: PASS')


if __name__ == '__main__':
    test_source()
