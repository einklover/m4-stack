#pragma once

// Executable Murphy M4 board contract.
//
// Source priority:
//   1. m4-device BoardConfig::MURPHY_M4 values independently live-probed on a
//      production unit.
//   2. SCH_ESP..18 schematic nets for physical-device presence/topology.
//   3. m4-firmware PlatformIO/partition settings for the current executable
//      flash target.
//
// Keep schematic-only devices separate from firmware-enabled capabilities.

#include <cstddef>
#include <cstdint>

namespace m4board {

struct MurphyM4Spec {
  static constexpr const char* kName = "murphy_m4";
  static constexpr const char* kMcu = "ESP32-S3R8";
  static constexpr std::size_t kPsramBytes = 8u * 1024u * 1024u;
  static constexpr std::size_t kFirmwareFlashBytes = 16u * 1024u * 1024u;
  static constexpr std::size_t kSchematicFlashNominalBytes = 4u * 1024u * 1024u;

  static constexpr uint16_t kDisplayWidth = 800;
  static constexpr uint16_t kDisplayHeight = 480;
  static constexpr std::size_t kFramebufferBytes =
      static_cast<std::size_t>(kDisplayWidth / 8) * kDisplayHeight;
  static constexpr uint32_t kDisplaySpiHz = 10000000;
  static constexpr int kEpdSclk = 4;
  static constexpr int kEpdMosi = 3;
  static constexpr int kEpdCs = 5;
  static constexpr int kEpdDc = 6;
  static constexpr int kEpdReset = 7;
  static constexpr int kEpdBusy = 8;
  static constexpr bool kEpdBusyActiveHigh = true;

  static constexpr int kSdPower = 10;
  static constexpr bool kSdPowerActiveHigh = false;
  static constexpr int kSdClk = 16;
  static constexpr int kSdCmd = 15;
  static constexpr int kSdD0 = 17;
  static constexpr int kSdD1 = 18;
  static constexpr int kSdD2 = 11;
  static constexpr int kSdD3 = 14;
  static constexpr unsigned kSdBusWidth = 4;

  static constexpr int kKeyUp = 1;
  static constexpr int kKeyDown = 2;
  static constexpr int kKeyLockPower = 0;
  static constexpr bool kKeysActiveLow = true;

  static constexpr int kBatteryAdc = 9;
  static constexpr float kBatteryDividerMultiplier = 2.0f;
  static constexpr int kChargeStatusSchematic = 43;  // firmware intentionally disabled

  static constexpr int kI2cSda = 13;
  static constexpr int kI2cScl = 12;
  static constexpr int kTouchIrq = 44;
  static constexpr int kTouchPower = 45;
  static constexpr bool kTouchPowerActiveHigh = false;
  static constexpr uint8_t kTouchAddress = 0x2E;
  static constexpr bool kTouchSwapXY = true;
  static constexpr bool kTouchFlipX = false;
  static constexpr bool kTouchFlipY = true;

  static constexpr int kBuzzer = 46;
  static constexpr int kFrontlightWarm = 47;
  static constexpr int kFrontlightCool = 48;
  static constexpr uint32_t kFrontlightPwmHz = 5000;
  static constexpr unsigned kFrontlightResolutionBits = 8;

  // Physical schematic devices on the shared I2C bus. BoardConfig currently
  // enables touch but leaves SensorsConfig as NO_SENSORS.
  static constexpr uint8_t kAht20Address = 0x38;
  static constexpr bool kAht20FirmwareEnabled = false;
  static constexpr bool kSc7a20FirmwareEnabled = false;
};

static_assert(MurphyM4Spec::kFramebufferBytes == 48000,
              "Murphy M4 SSD1677 framebuffer geometry changed unexpectedly");
static_assert(MurphyM4Spec::kSdBusWidth == 4, "Murphy M4 must use 4-bit SDMMC");
static_assert(MurphyM4Spec::kFirmwareFlashBytes != MurphyM4Spec::kSchematicFlashNominalBytes,
              "Known flash-size source discrepancy was silently erased");

}  // namespace m4board
