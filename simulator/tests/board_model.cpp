#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "board/MurphyBoard.h"
#include "board/MurphyM4Spec.h"

using namespace m4sim;

namespace {
int failures = 0;

void check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    ++failures;
  }
}

void test_board_wiring_and_power_polarity() {
  SimScheduler sched(1);
  SimTrace trace;
  MurphyBoard board(&sched, &trace);
  std::vector<std::string> errors;
  check(board.selfCheck(errors), "fresh board wiring must have no ownership/protocol conflicts");

  check(!board.sdPower().powered(), "SD rail starts off");
  check(board.gpio().read(m4board::MurphyM4Spec::kSdPower),
        "active-low SD gate must read HIGH while off");
  board.sdPower().setPowered(true);
  check(board.sdPower().powered(), "SD rail powers on");
  check(!board.gpio().read(m4board::MurphyM4Spec::kSdPower),
        "active-low SD gate must read LOW while on");

  check(!board.touchPower().powered(), "touch rail starts off");
  check(!board.i2c().probe(m4board::MurphyM4Spec::kTouchAddress),
        "unpowered touch must NACK");
  board.touchPower().setPowered(true);
  check(board.i2c().probe(m4board::MurphyM4Spec::kTouchAddress),
        "powered FT6x36-compatible touch must ACK at 0x2e");

  check(!board.i2c().probe(m4board::MurphyM4Spec::kAht20Address),
        "AHT20 is physically present but hidden from current firmware capability view");
  check(board.i2c().probe(m4board::MurphyM4Spec::kAht20Address, /*firmwareView=*/false),
        "physical-board probe must see schematic AHT20 at 0x38");
}

void test_gpio_conflict_detection() {
  SimGpio gpio;
  check(gpio.claim(5, "epd.cs"), "first GPIO owner claim succeeds");
  check(!gpio.claim(5, "sd.cs"), "unapproved GPIO alias must fail");
  check(!gpio.ok(), "GPIO conflict is retained as a diagnosable violation");
}

void write_full_frame(SimSpiBus& spi, uint8_t command, uint8_t value) {
  std::vector<uint8_t> frame(m4board::MurphyM4Spec::kFramebufferBytes, value);
  check(spi.writeCommand(command), "RAM command accepted");
  check(spi.writeData(frame), "full-frame RAM data accepted");
}

void test_ssd1677_atomic_activation_and_provenance() {
  SimScheduler sched(2);
  SimTrace trace;
  MurphyBoard board(&sched, &trace);
  auto& spi = board.displaySpi();
  auto& epd = board.epd();

  check(board.beginDisplayTransaction(), "start SSD1677 SPI transaction");
  check(spi.writeCommand(0x12), "soft reset command");
  sched.runFor(10);
  board.syncSignals();
  check(!epd.busy(), "soft reset BUSY clears after modeled reset interval");

  // New frame = black; differential old frame = white.
  write_full_frame(spi, 0x24, 0x00);
  write_full_frame(spi, 0x26, 0xFF);
  check(spi.writeCommand(0x21), "update control 1 command");
  check(spi.writeData(static_cast<uint8_t>(0x00)), "normal differential control");
  check(spi.writeCommand(0x22), "update control 2 command");
  check(spi.writeData(static_cast<uint8_t>(0xFC)), "Murphy/X4 fast sequence");
  check(spi.writeCommand(0x20), "master activation command");

  check(epd.busy(), "MASTER_ACTIVATION asserts BUSY");
  check(epd.activationCount() == 1, "one master activation recorded");
  check(epd.commitCount() == 0, "physical panel must not commit before BUSY completion");
  check(epd.lastChangedBits() == 800u * 480u,
        "BW-vs-RED differential counts every changed pixel in black/white frame");
  check(epd.physical().front() == 0xFF,
        "physical image remains previous frame while activation is BUSY");
  check(epd.activationCommitsAtomically(),
        "digital SSD1677 model exposes one commit boundary per activation");

  sched.runFor(420);
  board.syncSignals();
  check(!epd.busy(), "fast activation BUSY clears at calibrated duration");
  check(epd.commitCount() == 1, "activation commits exactly once");
  check(epd.physical().front() == 0x00, "physical frame becomes submitted BW RAM after commit");
  check(board.endDisplayTransaction(), "finish SSD1677 SPI transaction");
  check(epd.ok(), "valid SSD1677 sequence has no protocol errors");
}

void test_ssd1677_rejects_activation_while_busy() {
  SimScheduler sched(3);
  SimTrace trace;
  MurphyBoard board(&sched, &trace);
  auto& spi = board.displaySpi();
  auto& epd = board.epd();
  check(board.beginDisplayTransaction(), "start display transaction");
  check(spi.writeCommand(0x22), "ctrl2 command");
  check(spi.writeData(static_cast<uint8_t>(0xC0)), "power-on sequence data");
  check(spi.writeCommand(0x20), "first activation accepted");
  check(epd.busy(), "first activation busy");
  check(!spi.writeCommand(0x20), "second master activation during BUSY must be rejected");
  check(!epd.ok(), "BUSY protocol violation recorded");
  board.endDisplayTransaction();
}

void test_sd_power_loss_invalidates_inflight_io() {
  SimScheduler sched(4);
  SimTrace trace;
  MurphyBoard board(&sched, &trace);
  board.sdmmc().insert(4096);

  check(!board.sdmmc().mount(), "SD mount fails while active-low rail is off");

  // Use a fresh board so the expected negative test above does not pollute the
  // positive-path diagnostic state.
  MurphyBoard live(&sched, &trace);
  live.sdmmc().insert(4096);
  live.sdPower().setPowered(true);
  live.syncSignals();
  check(live.sdmmc().mount(4), "4-bit SDMMC mounts after rail enable");

  bool completed = false;
  bool readOk = true;
  check(live.sdmmc().readBlock(7, [&](bool ok) {
          completed = true;
          readOk = ok;
        }),
        "block read starts");
  live.sdPower().setPowered(false);
  live.syncSignals();
  sched.runFor(5);
  check(completed, "in-flight read completion fires after power loss");
  check(!readOk, "power loss makes in-flight SD read fail deterministically");
  check(!live.sdmmc().mounted(), "power loss invalidates mount");
}

}  // namespace

int main() {
  test_board_wiring_and_power_polarity();
  test_gpio_conflict_detection();
  test_ssd1677_atomic_activation_and_provenance();
  test_ssd1677_rejects_activation_while_busy();
  test_sd_power_loss_invalidates_inflight_io();
  if (failures) {
    std::cerr << failures << " board-model test(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "PASS Murphy M4 low-level board model\n";
  return EXIT_SUCCESS;
}
