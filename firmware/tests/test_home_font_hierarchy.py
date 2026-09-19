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
    # Target logo ink h~17, need bold weight and 16+ size; keep position but increase weight
    assert n["font"] == "ui_16_bold", f"Murphy M4 font {n['font']} != ui_16_bold (target hierarchy bold)"
    assert n["rect"][3] >= 24 and n["rect"][3] <= 26, f"logo rect h {n['rect'][3]} not 24-26"

def test_current_title_prominence():
    n = _find(_nodes(), text="$current.title")
    # Title should be most prominent: 20_bold (larger than 18) per target title larger than metadata
    assert n["font"] in ("ui_20_bold", "ui_18_bold"), f"title font {n['font']} not prominent"
    # Rect height should be >=28 to allow larger glyphs without clipping; allow <=2px variance
    assert 28 <= n["rect"][3] <= 32, f"title rect h {n['rect'][3]} not 28-32 for baseline"

def test_section_headers_hierarchy():
    for text in ("最近阅读", "应用"):
        n = _find(_nodes(), text=text)
        # Section headers should be bolder/larger than metadata (14) => 16_bold
        assert n["font"] == "ui_16_bold", f"{text} font {n['font']} != ui_16_bold"

def test_metadata_and_progress_hierarchy():
    for text in ("$current.author", "$current.source"):
        n = _find(_nodes(), text=text)
        assert n["font"] == "ui_14_regular", f"{text} font {n['font']}"
        assert n["rect"][3] == 18
    n = _find(_nodes(), text="继续阅读")
    assert n["font"] == "ui_14_regular"
    n = _find(_nodes(), text="$home.current.progress_text")
    assert n["font"] == "ui_12_regular"

def test_recent_and_app_labels():
    n = _find(_nodes(), source="$recent")
    child = n["children"][1]
    assert child["font"] == "ui_14_regular", f"recent title font {child['font']}"
    assert child["rect"][3] == 18
    n = _find(_nodes(), source="$apps")
    child = n["children"][1]
    assert child["font"] == "ui_12_regular"
    assert child["rect"][3] == 18

def test_battery_percentage_font():
    # Battery percentage is via battery node, not text; ensure battery node still present and logo baseline polished
    nodes = _nodes()
    logo = _find(nodes, text="Murphy M4")
    assert logo["rect"][0] == 24 and logo["rect"][1] == 24, "logo position must be 24,24 (polished to ink 24±2)"
    bat = [n for n in nodes if n.get("type")=="battery"][0]
    assert bat["rect"] == [432,24,24,12], "battery geometry unchanged"
