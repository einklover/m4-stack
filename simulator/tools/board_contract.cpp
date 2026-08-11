#include <iostream>

#include "board/MurphyM4Spec.h"

using m4board::MurphyM4Spec;

int main() {
  // Keep this dependency-free so CI, QEMU patch tooling and external agents can
  // consume the exact constants compiled into deterministic M4Sim.
  std::cout
      << "{\n"
      << "  \"schema\": 1,\n"
      << "  \"board\": \"" << MurphyM4Spec::kName << "\",\n"
      << "  \"soc\": {\"model\": \"" << MurphyM4Spec::kMcu
      << "\", \"psram_bytes\": " << MurphyM4Spec::kPsramBytes
      << ", \"firmware_flash_bytes\": " << MurphyM4Spec::kFirmwareFlashBytes
      << ", \"schematic_flash_nominal_bytes\": "
      << MurphyM4Spec::kSchematicFlashNominalBytes << "},\n"
      << "  \"display\": {\"controller\": \"SSD1677\", \"width\": "
      << MurphyM4Spec::kDisplayWidth << ", \"height\": " << MurphyM4Spec::kDisplayHeight
      << ", \"framebuffer_bytes\": " << MurphyM4Spec::kFramebufferBytes
      << ", \"spi_hz\": " << MurphyM4Spec::kDisplaySpiHz
      << ", \"gpio\": {\"sclk\": " << MurphyM4Spec::kEpdSclk
      << ", \"mosi\": " << MurphyM4Spec::kEpdMosi
      << ", \"cs\": " << MurphyM4Spec::kEpdCs
      << ", \"dc\": " << MurphyM4Spec::kEpdDc
      << ", \"reset\": " << MurphyM4Spec::kEpdReset
      << ", \"busy\": " << MurphyM4Spec::kEpdBusy << "}},\n"
      << "  \"sdmmc\": {\"bus_width\": " << MurphyM4Spec::kSdBusWidth
      << ", \"block_bytes\": 512, \"gpio\": {\"power\": " << MurphyM4Spec::kSdPower
      << ", \"clk\": " << MurphyM4Spec::kSdClk
      << ", \"cmd\": " << MurphyM4Spec::kSdCmd
      << ", \"d0\": " << MurphyM4Spec::kSdD0
      << ", \"d1\": " << MurphyM4Spec::kSdD1
      << ", \"d2\": " << MurphyM4Spec::kSdD2
      << ", \"d3\": " << MurphyM4Spec::kSdD3
      << ", \"card_detect\": null}, \"power_active_high\": "
      << (MurphyM4Spec::kSdPowerActiveHigh ? "true" : "false") << "},\n"
      << "  \"keys\": {\"active_low\": " << (MurphyM4Spec::kKeysActiveLow ? "true" : "false")
      << ", \"up\": " << MurphyM4Spec::kKeyUp
      << ", \"down\": " << MurphyM4Spec::kKeyDown
      << ", \"lock_power\": " << MurphyM4Spec::kKeyLockPower << "},\n"
      << "  \"touch\": {\"family\": \"FT6x36-compatible\", \"i2c_address\": "
      << static_cast<unsigned>(MurphyM4Spec::kTouchAddress)
      << ", \"sda\": " << MurphyM4Spec::kI2cSda
      << ", \"scl\": " << MurphyM4Spec::kI2cScl
      << ", \"irq\": " << MurphyM4Spec::kTouchIrq
      << ", \"power\": " << MurphyM4Spec::kTouchPower
      << ", \"power_active_high\": "
      << (MurphyM4Spec::kTouchPowerActiveHigh ? "true" : "false") << "},\n"
      << "  \"battery\": {\"adc_gpio\": " << MurphyM4Spec::kBatteryAdc
      << ", \"divider_multiplier\": " << MurphyM4Spec::kBatteryDividerMultiplier << "},\n"
      << "  \"frontlight\": {\"warm_gpio\": " << MurphyM4Spec::kFrontlightWarm
      << ", \"cool_gpio\": " << MurphyM4Spec::kFrontlightCool
      << ", \"pwm_hz\": " << MurphyM4Spec::kFrontlightPwmHz
      << ", \"resolution_bits\": " << MurphyM4Spec::kFrontlightResolutionBits << "},\n"
      << "  \"buzzer_gpio\": " << MurphyM4Spec::kBuzzer << ",\n"
      << "  \"physical_i2c\": {\"aht20\": {\"address\": "
      << static_cast<unsigned>(MurphyM4Spec::kAht20Address)
      << ", \"firmware_enabled\": "
      << (MurphyM4Spec::kAht20FirmwareEnabled ? "true" : "false")
      << "}, \"sc7a20_firmware_enabled\": "
      << (MurphyM4Spec::kSc7a20FirmwareEnabled ? "true" : "false") << "},\n"
      << "  \"unresolved\": [\"sd_card_detect_gpio\", \"touch_exact_part_number\", "
         "\"u6_i2c_part_number\", \"shipping_flash_jedec_id\"]\n"
      << "}\n";
  return 0;
}
