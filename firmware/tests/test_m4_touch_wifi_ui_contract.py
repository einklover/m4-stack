from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
keyboard = (ROOT / "src/activities/util/KeyboardEntryActivity.cpp").read_text(encoding="utf-8")
wifi = (ROOT / "src/activities/network/WifiSelectionActivity.cpp").read_text(encoding="utf-8")

# The keyboard is a touch-first implementation, not the old 13-column/D-pad UI.
assert '#include "util/TouchUiGeometry.h"' in keyboard
assert "makeTouchKeyboardLayout" in keyboard
assert "TouchKeyboardState" in keyboard
assert "passwordRevealed" in keyboard
assert "qrButtonRect" in keyboard

loop = keyboard.split("void KeyboardEntryActivity::loop()", 1)[1].split(
    "void KeyboardEntryActivity::render()", 1
)[0]
assert "wasScreenTapped" in loop
for legacy_button in (
    "Button::Up",
    "Button::Down",
    "Button::Left",
    "Button::Right",
    "Button::Confirm",
):
    assert legacy_button not in loop, f"legacy physical navigation remains: {legacy_button}"

# Wi-Fi selection and rendering use the same large touch geometry, with an explicit rescan target.
assert '#include "util/TouchUiGeometry.h"' in wifi
assert "makeWifiNetworkListLayout" in wifi
assert "refresh.contains" in wifi
assert "lineHeight = 25" not in wifi

wifi_loop = wifi.split("void WifiSelectionActivity::loop()", 1)[1].split(
    "std::string WifiSelectionActivity::getSignalStrengthIndicator", 1
)[0]
assert "wasScreenTapped" in wifi_loop
assert "wasSwipe" in wifi_loop

wifi_render = wifi.split("void WifiSelectionActivity::renderNetworkList() const", 1)[1].split(
    "void WifiSelectionActivity::renderConnecting() const", 1
)[0]
assert "makeWifiNetworkListLayout" in wifi_render
assert "GUI.drawButtonHints" not in wifi_render

# Wi-Fi password entry is masked by default on a touch device.
password_launch = wifi.split("new KeyboardEntryActivity(", 1)[1].split("));", 1)[0]
assert "true" in password_launch, "Wi-Fi password entry must be masked by default"
