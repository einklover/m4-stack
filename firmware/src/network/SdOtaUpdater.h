#pragma once

#include <functional>
#include <string>

/**
 * SD-card based OTA updater — two-stage scheme.
 *
 * Stage 1 (main firmware, this class):
 *   1. checkSdCard()       – verify /update/firmware.bin exists on SD
 *   2. flashUpdaterAndReboot() – write minimal updater to app1, switch boot, reboot
 *
 * Stage 2 (updater firmware in app1):
 *   - Reads /update/firmware.bin from SD
 *   - Writes it to app0 via esp_ota API
 *   - Switches boot back to app0, reboots
 */
class SdOtaUpdater {
 public:
  enum Error {
    OK = 0,
    NO_UPDATE,
    SD_ERROR,
    FLASH_ERROR,
  };

  using ProgressCallback = std::function<void(size_t done, size_t total)>;

  SdOtaUpdater() = default;

  Error checkSdCard();
  size_t getFirmwareSize() const { return firmwareSize; }

  /** Write updater firmware to app1, set boot to app1, caller must reboot. */
  Error flashUpdaterAndReboot(ProgressCallback progress = nullptr);

 private:
  static constexpr const char* SD_FW_PATH = "/update/firmware.bin";
  size_t firmwareSize = 0;
};
