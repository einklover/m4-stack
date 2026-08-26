#!/usr/bin/env python3
"""Static contract: Murphy M4 boots directly to Home or Reader."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAIN = ROOT / "firmware" / "src" / "main.cpp"


def test_m4_direct_boot_contract() -> None:
    source = MAIN.read_text(encoding="utf-8")

    assert '#include "activities/boot_sleep/BootActivity.h"' not in source
    assert "enterNewActivity(new BootActivity" not in source

    # The existing state restore and direct Home/Reader routing must remain.
    setup = source[source.index("void setup()") : source.index("void loop()")]
    load_at = setup.index("APP_STATE.loadFromFile();")
    recent_at = setup.index("RECENT_BOOKS.loadFromFile();")
    assert load_at < recent_at
    routing = setup[recent_at:]
    assert "onGoHome();" in routing
    assert "onGoToReader(path, originalSourcePath);" in routing


if __name__ == "__main__":
    test_m4_direct_boot_contract()
    print("m4 direct boot contract: PASS")
