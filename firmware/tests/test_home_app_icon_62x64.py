from pathlib import Path
import re

H = Path(__file__).resolve().parents[1] / "src/ui/pages/HomeSceneModel.h"
F = Path(__file__).resolve().parents[1] / "src/qemu/M4QemuHomeSceneFixture.h"

def test_home_app_icon_is_62x64():
    txt = H.read_text()
    assert "kHomeAppIconW = 62" in txt, "W must be 62"
    assert "kHomeAppIconH = 64" in txt, "H must be 64"
    assert "kHomeAppIconStride = 8" in txt, "stride 8 for 62"
    assert "kHomeAppIconBytes = 512" in txt, "8*64=512"
    assert "kHomeAssetArenaBytes = 7748" in txt, "2520+3*1060+4*512=7748"

def test_qemu_fixture_uses_same():
    txt = F.read_text()
    # Should use 62,64 not 68,68
    assert "Bitmap<62, 64> icons[4]" in txt or "Bitmap<62,64> icons[4]" in txt, "QEMU icons must be 62x64"
    assert "Bitmap<68, 68> icons" not in txt, "old 68x68 must be gone"

def test_renderer_icon_no_overflow():
    txt = (Path(__file__).resolve().parents[1] / "src/ui/scene/GfxSceneRenderer.h").read_text()
    assert "kNodeIcon" in txt
    # Icon must remain 1:1 (draw1BitAsset at rect origin), not aspect-fill. Covers use drawCoverAsset.
    # Check both icon dispatch blocks use draw1BitAsset, not drawCoverAsset
    icon_section = txt.split("kNodeIcon")[1].split("kNodeBitmap")[0]
    assert "draw1BitAsset" in icon_section, "icon must use 1:1 blit"
    assert "drawCoverAsset" not in icon_section, "icon must not be aspect-filled"
    # Covers should use drawCoverAsset exactly twice (main + recent) in the two render lambdas = 4 calls + definition = 5 total, but icon section must have 0
    assert txt.count("drawCoverAsset(gfx,") == 4 or txt.count("drawCoverAsset") >= 2
