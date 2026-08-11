#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "board/MurphyBoard.h"
#include "hardware/SimBattery.h"
#include "hardware/SimFrontlight.h"
#include "hardware/SimInput.h"

using namespace m4sim;

namespace {
int failures = 0;
void check(bool v, const std::string& m) {
  if (!v) { std::cerr << "FAIL: " << m << "\n"; ++failures; }
}

void test_battery_policy() {
  SimBattery b(2.0f);
  b.setCellMillivolts(4000);
  check(b.adcMillivolts() == 2000, "2:1 divider exposes half cell mV to ADC");
  check(b.productionReadMillivolts() == 4000, "production multiplier restores cell voltage");
  check(b.percentage() == SimBattery::percentageFromMillivolts(4000),
        "battery percentage follows production cubic curve");
  b.setCellMillivolts(2500);
  check(b.percentage() <= 100, "battery percentage remains clamped");
  b.setCellMillivolts(5000);
  check(b.percentage() <= 100, "over-range voltage cannot report >100 percent");
}

void test_frontlight_split() {
  SimFrontlight fl(5000, 8, true);
  fl.setBrightness(80);
  fl.setWarmPercent(25);
  const auto d = fl.duty();
  check(d.cool == (60u * 255u) / 100u, "80% brightness / 25% warm -> 60% cool duty");
  check(d.warm == (20u * 255u) / 100u, "80% brightness / 25% warm -> 20% warm duty");
  fl.off();
  check(fl.duty().cool == 0 && fl.duty().warm == 0, "frontlight off drives both channels to zero");
  fl.on();
  check(fl.brightness() == 80, "frontlight on restores last nonzero brightness");
}

void test_active_low_button_debounce() {
  SimInput input(30);
  input.sampleButtons(0, true, true, true);
  input.sampleButtons(10, false, true, true);
  input.sampleButtons(25, true, true, true);
  input.sampleButtons(40, false, true, true);
  input.sampleButtons(71, false, true, true);
  check(input.wasPressed(MurphyButton::Up), "stable active-low UP emits one debounced press edge");
  check(input.pressed(MurphyButton::Up), "UP remains committed pressed");
  input.sampleButtons(80, true, true, true);
  input.sampleButtons(111, true, true, true);
  check(input.wasReleased(MurphyButton::Up), "stable high release emits one edge");
}

void test_touch_down_routing_and_swipe() {
  SimInput input;
  input.touchDown({100, 100, 0});
  input.touchMove({110, 108, 40});
  input.touchUp({116, 112, 80});
  auto tap = input.consumeTap();
  check(tap.has_value(), "small motion remains a tap");
  if (tap) {
    check(std::fabs(tap->x - 100.0f / 799.0f) < 0.0001f,
          "tap route uses first-contact X, not lift-off centroid");
    check(std::fabs(tap->y - 100.0f / 479.0f) < 0.0001f,
          "tap route uses first-contact Y");
  }

  input.touchDown({100, 200, 100});
  input.touchMove({300, 205, 180});
  input.touchUp({500, 210, 240});
  check(!input.consumeTap().has_value(), "large movement invalidates tap candidate");
  auto swipe = input.consumeSwipe();
  check(swipe.has_value(), "fast long movement becomes swipe");
  if (swipe) check(swipe->end.x > swipe->start.x, "swipe preserves movement direction");
}

void test_ft6336_silicon_transform_and_frames() {
  auto p = SimInput::ft6336RawToPanel(/*rawX=*/0, /*rawY=*/799, 12);
  check(p.has_value() && p->x == 799 && p->y == 479,
        "M4 swapXY+flipY maps raw portrait corner into panel frame");
  auto q = SimInput::ft6336RawToPanel(/*rawX=*/479, /*rawY=*/0, 13);
  check(q.has_value() && q->x == 0 && q->y == 0,
        "opposite raw corner maps to panel origin");
  check(!SimInput::ft6336RawToPanel(480, 0, 0).has_value(),
        "out-of-range FT6336 raw X is rejected");
  check(!SimInput::ft6336RawToPanel(0, 800, 0).has_value(),
        "out-of-range FT6336 raw Y is rejected");

  SimFt6x36Device dev;
  check(dev.setPoint(123, 456, SimFt6x36Device::Event::Contact),
        "valid raw touch point encodes into FT frame");
  check(dev.write({0}), "InputManager register-0 pointer write accepted");
  std::vector<uint8_t> frame;
  check(dev.read(8, frame) && frame.size() == 8 && (frame[2] & 0x0F) == 1,
        "production 8-byte FT frame read reports one point");
  const uint16_t rawX = static_cast<uint16_t>(((frame[3] & 0x0F) << 8) | frame[4]);
  const uint16_t rawY = static_cast<uint16_t>(((frame[5] & 0x0F) << 8) | frame[6]);
  check(rawX == 123 && rawY == 456, "FT frame preserves 12-bit raw coordinates");
  dev.failNextReads(1);
  check(!dev.read(8, frame), "touch controller can inject bus/read failure");
}

void test_deep_sleep_rails_and_power_wake() {
  SimScheduler sched(9);
  SimTrace trace;
  MurphyBoard board(&sched, &trace);
  board.sdPower().setPowered(true);
  board.touchPower().setPowered(true);
  // No external key drive means the configured pull-up is the released level.
  check(board.sleepPower().enterDeepSleep(), "released active-low power key permits deep sleep");
  check(board.sleepPower().sleeping(), "board enters simulated deep sleep");
  check(!board.sdPower().powered() && !board.touchPower().powered(),
        "PowerManager drives active-low SD/touch rails HIGH/off before sleep");
  check(board.sleepPower().railsHeld(), "rail-off levels are held through deep sleep");
  check(board.sleepPower().injectPowerButton(true),
        "GPIO0 active-low press triggers S3 low-level wake");
  check(board.sleepPower().wakeReason() == SimSleepPower::WakeReason::PowerButton,
        "wake reason preserves power-button provenance");
  check(!board.sdPower().powered() && !board.touchPower().powered(),
        "held rails stay off immediately after wake until boot reinitializes them");
}
}

int main() {
  test_battery_policy();
  test_frontlight_split();
  test_active_low_button_debounce();
  test_touch_down_routing_and_swipe();
  test_ft6336_silicon_transform_and_frames();
  test_deep_sleep_rails_and_power_wake();
  if (failures) return EXIT_FAILURE;
  std::cout << "PASS Murphy peripheral policies\n";
  return EXIT_SUCCESS;
}
