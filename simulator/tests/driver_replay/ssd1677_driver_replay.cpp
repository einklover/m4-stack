#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "HostEpdBridge.h"
#include "board/MurphyM4Spec.h"
#include "core/SimKernel.h"
#include "hardware/SimSpiBus.h"
#include "hardware/SimSsd1677Controller.h"
#include "bus/EpdBus.h"
#include "driver/Ssd1677Driver.h"

using namespace m4sim;
using namespace freeink;

namespace {
int failures = 0;
void check(bool ok, const std::string& message) {
  if (!ok) {
    std::cerr << "FAIL: " << message << "\n";
    ++failures;
  }
}
}

int main() {
  SimScheduler scheduler(77);
  SimTrace trace;
  SimSpiBus spi(m4board::MurphyM4Spec::kEpdSclk,
                m4board::MurphyM4Spec::kEpdMosi);
  SimSsd1677Controller controller(&scheduler, &trace);
  check(spi.attach(m4board::MurphyM4Spec::kEpdCs, "ssd1677-real-driver",
                   &controller, 20000000),
        "attach real-driver replay to simulated controller");

  hostreplay::Context replay{&scheduler, &spi, &controller,
                             m4board::MurphyM4Spec::kEpdCs,
                             m4board::MurphyM4Spec::kDisplaySpiHz, false, true};
  hostreplay::bind(&replay);

  EpdBus bus;
  const EpdPins pins{m4board::MurphyM4Spec::kEpdSclk,
                     m4board::MurphyM4Spec::kEpdMosi,
                     m4board::MurphyM4Spec::kEpdCs,
                     m4board::MurphyM4Spec::kEpdDc,
                     m4board::MurphyM4Spec::kEpdReset,
                     m4board::MurphyM4Spec::kEpdBusy,
                     -1};
  Ssd1677Driver driver;
  bus.begin(pins, driver.spiHz(), driver.busyPolarity());

  check(driver.geometry().width == 800 && driver.geometry().height == 480,
        "real FreeInk driver compiles with Murphy 800x480 BoardConfig");
  check(driver.spiHz() == 10000000,
        "real FreeInk driver consumes Murphy 10MHz live-probed SPI setting");

  driver.begin(bus);
  check(replay.protocolOk, "real init stream obeys host EpdBus transaction contract");
  check(controller.ok(), "real init stream is accepted by SSD1677 register model");
  check(controller.lastRamExpected() == m4board::MurphyM4Spec::kFramebufferBytes,
        "real driver programs full 800x480 RAM window");

  const size_t bytes = m4board::MurphyM4Spec::kFramebufferBytes;
  std::vector<uint8_t> white(bytes, 0xFF);
  std::vector<uint8_t> black(bytes, 0x00);

  driver.display(bus, black.data(), white.data(), RefreshMode::Fast, false);
  check(replay.protocolOk && controller.ok(),
        "real first-paint command stream is controller-valid");
  check(controller.commitCount() == 1,
        "real first paint produces exactly one physical activation commit");
  check(controller.lastVisibleInversionPhases() == 0 && controller.lastFullWaveformPhases() == 0,
        "first fast paint has no visible inversion/full-waveform phases");
  check(controller.physical().front() == 0x00 && controller.physical().back() == 0x00,
        "real first paint commits black framebuffer provenance");

  driver.display(bus, white.data(), black.data(), RefreshMode::Fast, false);
  check(replay.protocolOk && controller.ok(),
        "real fast-update stream is controller-valid");
  check(controller.updateCtrl1() == 0x00,
        "real fast update selects normal current-vs-old differential semantics");
  check(controller.updateCtrl2() == 0xFC,
        "real Murphy default fast path retains stock 0xFC update sequence");
  check(controller.commitCount() == 2,
        "second real driver refresh adds exactly one commit");
  check(controller.physical().front() == 0xFF && controller.physical().back() == 0xFF,
        "real fast update lands the submitted white frame");

  driver.display(bus, black.data(), white.data(), RefreshMode::Full, false);
  check(controller.updateCtrl2() == 0xFC,
        "legacy raw FULL request is normalized to the fast sequence");
  check(controller.lastVisibleInversionPhases() == 0 && controller.lastFullWaveformPhases() == 0,
        "legacy raw FULL request cannot trigger a visible/full waveform");

  driver.display(bus, white.data(), black.data(), RefreshMode::ReaderCleanup, false);
  check(controller.updateCtrl2() == 0xD7,
        "explicit reader cleanup selects the single-pass D7 sequence");
  check(controller.lastVisibleInversionPhases() == 1 && controller.lastFullWaveformPhases() == 0,
        "reader cleanup has at most one visible inversion and no full waveform");

  check(spi.ok(), "real driver did not violate simulated SPI bus discipline");

  if (failures) {
    std::cerr << failures << " real-driver replay test(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "PASS real FreeInk Ssd1677Driver -> SimSsd1677Controller replay\n";
  return EXIT_SUCCESS;
}
