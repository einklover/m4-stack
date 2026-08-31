import json, pathlib
THEME = pathlib.Path(__file__).resolve().parents[2] / "themes/murphy-default/theme.json"
def test_no_long_line_y540_624():
    data=json.loads(THEME.read_text())
    for n in data["nodes"]:
        if n.get("type")=="line":
            y=n.get("y", n.get("rect",[0,0])[1] if "rect" in n else 0)
            x=n.get("x",0); x2=n.get("x2",0)
            w= abs(x2 - x) if "x2" in n else n.get("rect",[0,0,0,0])[2]
            if 540 <= y <= 624 and w> 200:
                assert False, f"long divider at y={y} x={x}..{x2} violates no-line 540..624"
