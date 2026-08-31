import json
from pathlib import Path

THEME = Path(__file__).resolve().parents[2] / "themes/murphy-default/theme.json"

def _nodes():
    return json.loads(THEME.read_text(encoding="utf-8"))["nodes"]

def _find(nodes, text=None, binding=None, source=None):
    for n in nodes:
        if text is not None and n.get("text") != text:
            continue
        if binding is not None and n.get("binding") != binding:
            continue
        if source is not None and n.get("source") != source:
            continue
        return n
    raise AssertionError(f"not found text={text} binding={binding} source={source}")

def test_murphy_logo_font_weight_and_height():
    n = _find(_nodes(), text="Murphy M4")
    # HomeRef header role: bold 22px while keeping the measured header rect.
    assert n["font"] == "ui_22_bold", f"Murphy M4 font {n['font']} != ui_22_bold"
    assert n["rect"][3] >= 24 and n["rect"][3] <= 26, f"logo rect h {n['rect'][3]} not 24-26"

def test_current_title_prominence():
    n = _find(_nodes(), text="$current.title")
    # HomeRef hero role: 24px bold and larger than all metadata.
    assert n["font"] == "ui_24_bold", f"title font {n['font']} != ui_24_bold"
    # Two-line title slot (~52px) so long book names wrap before ellipsis
    assert 48 <= n["rect"][3] <= 56, f"title rect h {n['rect'][3]} not 48-56 for 2-line wrap"

def test_section_headers_hierarchy():
    for text in ("最近阅读", "应用"):
        n = _find(_nodes(), text=text)
        # HomeRef section role: 20px bold, distinct from 16px metadata.
        assert n["font"] == "ui_20_bold", f"{text} font {n['font']} != ui_20_bold"

def test_metadata_and_progress_hierarchy():
    for text in ("$current.author", "$current.source"):
        n = _find(_nodes(), text=text)
        assert n["font"] == "ui_16_regular", f"{text} font {n['font']}"
        assert n["rect"][3] == 18
    n = _find(_nodes(), text="继续阅读")
    assert n["font"] == "ui_16_regular"
    n = _find(_nodes(), text="$home.current.progress_text")
    assert n["font"] == "ui_16_regular"

def test_recent_and_app_labels():
    n = _find(_nodes(), source="$recent")
    child = n["children"][1]
    assert child["font"] == "ui_16_regular", f"recent title font {child['font']}"
    assert child["rect"][3] == 44
    n = _find(_nodes(), source="$apps")
    child = n["children"][1]
    assert child["font"] == "ui_16_regular"
    assert child["rect"][3] == 18

def test_battery_percentage_font():
    # Battery percentage is via battery node, not text; ensure battery node still present and logo baseline polished
    nodes = _nodes()
    logo = _find(nodes, text="Murphy M4")
    assert logo["rect"][0] == 24 and logo["rect"][1] == 24, "logo position must be 24,24 (polished to ink 24±2)"
    bat = [n for n in nodes if n.get("type")=="battery"][0]
    assert bat["rect"] == [432,24,24,12], "battery geometry unchanged"
