#include <cstdlib>
#include <iostream>
#include <string>

#include "core/SimKernel.h"
#include "hardware/SimSdmmc.h"

using namespace m4sim;

namespace {
int failures = 0;
void check(bool value, const std::string& message) {
  if (!value) {
    std::cerr << "FAIL: " << message << "\n";
    ++failures;
  }
}

void test_sector_roundtrip() {
  SimScheduler sched(101);
  SimTrace trace;
  bool power = true;
  SimSdmmc sd(&sched, &trace, SimSdmmc::Config{-1, -1, -1, -1, -1, -1, 4, 2, 3},
              [&] { return power; });
  sd.insert(32);
  check(sd.cardDetectAsserted(), "inserted image asserts logical card detect");
  check(sd.blocks() == 32, "capacity is reported in 512-byte sectors");
  check(sd.mount(4), "image-backed card mounts on matching 4-bit bus");

  SimSdmmc::Block expected{};
  for (std::size_t i = 0; i < expected.size(); ++i) {
    expected[i] = static_cast<uint8_t>((i * 17u + 3u) & 0xffu);
  }

  bool writeDone = false;
  bool writeOk = false;
  check(sd.writeBlockData(7, expected, [&](bool ok) {
          writeDone = true;
          writeOk = ok;
        }),
        "sector write schedules");
  sched.runFor(3);
  check(writeDone && writeOk, "sector write completes successfully");
  check(sd.image().dirty(), "successful write marks backing image dirty");

  bool readDone = false;
  bool readOk = false;
  SimSdmmc::Block actual{};
  check(sd.readBlockData(7, [&](bool ok, const SimSdmmc::Block& block) {
          readDone = true;
          readOk = ok;
          actual = block;
        }),
        "sector read schedules");
  sched.runFor(2);
  check(readDone && readOk, "sector read completes successfully");
  check(actual == expected, "read sector equals bytes written by guest");
}

void test_card_remove_invalidates_write() {
  SimScheduler sched(102);
  SimTrace trace;
  bool power = true;
  SimSdmmc sd(&sched, &trace, SimSdmmc::Config{-1, -1, -1, -1, -1, -1, 4, 1, 4},
              [&] { return power; });
  sd.insert(8);
  check(sd.mount(), "fixture mounts");
  SimSdmmc::Block block{};
  block.fill(0xa5);
  bool done = false;
  bool ok = true;
  check(sd.writeBlockData(2, block, [&](bool result) {
          done = true;
          ok = result;
        }),
        "write starts before removal");
  sd.remove();
  check(!sd.cardDetectAsserted(), "removal deasserts logical card detect");
  sched.runFor(4);
  check(done && !ok, "removal invalidates the in-flight write generation");
}

void test_power_loss_invalidates_data_read() {
  SimScheduler sched(103);
  SimTrace trace;
  bool power = true;
  SimSdmmc sd(&sched, &trace, SimSdmmc::Config{-1, -1, -1, -1, -1, -1, 4, 5, 2},
              [&] { return power; });
  sd.insert(8);
  check(sd.mount(), "fixture mounts before power race");
  bool done = false;
  bool ok = true;
  check(sd.readBlockData(0, [&](bool result, const SimSdmmc::Block&) {
          done = true;
          ok = result;
        }),
        "read starts before rail drop");
  power = false;
  sd.notifyPowerChanged();
  sched.runFor(5);
  check(done && !ok, "rail drop invalidates in-flight payload read");
  check(!sd.mounted(), "power loss requires remount");
}
}  // namespace

int main() {
  test_sector_roundtrip();
  test_card_remove_invalidates_write();
  test_power_loss_invalidates_data_read();
  if (failures) return EXIT_FAILURE;
  std::cout << "PASS image-backed Murphy SDMMC model\n";
  return EXIT_SUCCESS;
}
