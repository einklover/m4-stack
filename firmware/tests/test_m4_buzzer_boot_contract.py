#!/usr/bin/env python3
"""Static contract for the M4 cross-firmware buzzer handoff."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
MAIN = ROOT / "firmware" / "src" / "main.cpp"


def test_m4_buzzer_is_sanitized_before_normal_startup() -> None:
    source = MAIN.read_text(encoding="utf-8")

    helper = re.search(
        r"static void sanitizeM4BuzzerEarly\(\)\s*\{(?P<body>.*?)\n\}",
        source,
        re.DOTALL,
    )
    assert helper, "M4 early buzzer sanitizer is missing"
    body = helper.group("body")

    assert "kM4BuzzerPin = 46" in body
    assert "kM4BuzzerInactiveLevel = LOW" in body
    assert "gpio_hold_dis" in body
    assert "ledcDetach" in body
    assert "gpio_reset_pin" in body
    assert "gpio_set_pull_mode" in body
    assert "GPIO_PULLDOWN_ONLY" in body
    assert body.count("gpio_set_level") >= 2
    assert "gpio_set_direction" in body
    assert "gpio_deep_sleep_hold_dis" not in body
    assert "ledc_reset" not in body
    assert "GPIO_NUM_0" not in body

    # Disconnect the inherited peripheral route before making the pin an output;
    # preload LOW before enabling the output to avoid an active-high glitch.
    order = [
        "gpio_hold_dis",
        "ledcDetach",
        "gpio_reset_pin",
        "gpio_set_pull_mode",
        "gpio_set_level",
        "gpio_set_direction",
        "gpio_set_level",
    ]
    positions = []
    cursor = 0
    for token in order:
        position = body.index(token, cursor)
        positions.append(position)
        cursor = position + len(token)
    assert positions == sorted(positions), positions

    setup = re.search(r"void setup\(\)\s*\{(?P<body>.*?)\n\}", source, re.DOTALL)
    assert setup, "setup() is missing"
    setup_body = setup.group("body")
    sanitize_at = setup_body.index("sanitizeM4BuzzerEarly();")
    assert sanitize_at < setup_body.index("Serial.begin")
    assert sanitize_at < setup_body.index("delay(")
    assert sanitize_at < setup_body.index("gpio.begin")

    marker_at = setup_body.index("[M4-BUZZER] early sanitize complete")
    assert marker_at > setup_body.index("Serial.begin")


if __name__ == "__main__":
    test_m4_buzzer_is_sanitized_before_normal_startup()
    print("m4 buzzer boot contract: PASS")
