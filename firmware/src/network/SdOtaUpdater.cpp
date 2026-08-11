#include "SdOtaUpdater.h"

#include <SDCardManager.h>
#include "esp_ota_ops.h"
#include "esp_partition.h"

// Embedded updater firmware binary (built from updater/ project)
extern const uint8_t updater_fw_start[] asm("_binary_src_network_updater_fw_bin_start");
extern const uint8_t updater_fw_end[]   asm("_binary_src_network_updater_fw_bin_end");

SdOtaUpdater::Error SdOtaUpdater::checkSdCard() {
  if (!SdMan.ready()) {
    Serial.printf("[%lu] [SdOTA] SD card not ready\n", millis());
    return SD_ERROR;
  }

  if (!SdMan.exists(SD_FW_PATH)) {
    Serial.printf("[%lu] [SdOTA] %s not found\n", millis(), SD_FW_PATH);
    return NO_UPDATE;
  }

  FsFile f;
  if (!SdMan.openFileForRead("SdOTA", SD_FW_PATH, f)) return SD_ERROR;
  firmwareSize = f.fileSize();
  f.close();

  if (firmwareSize == 0) {
    Serial.printf("[%lu] [SdOTA] firmware.bin is empty\n", millis());
    return SD_ERROR;
  }

  Serial.printf("[%lu] [SdOTA] Found firmware.bin (%u bytes)\n", millis(), firmwareSize);
  return OK;
}

SdOtaUpdater::Error SdOtaUpdater::flashUpdaterAndReboot(ProgressCallback progress) {
  const size_t updaterSize = updater_fw_end - updater_fw_start;
  Serial.printf("[%lu] [SdOTA] Updater firmware size: %u bytes\n", millis(), updaterSize);

  // Find app1 (ota_1) partition
  const esp_partition_t* app1 = esp_ota_get_next_update_partition(nullptr);
  if (!app1) {
    Serial.printf("[%lu] [SdOTA] No next update partition found\n", millis());
    return FLASH_ERROR;
  }

  if (updaterSize > app1->size) {
    Serial.printf("[%lu] [SdOTA] Updater (%u) > partition (%u)\n",
                  millis(), updaterSize, app1->size);
    return FLASH_ERROR;
  }

  Serial.printf("[%lu] [SdOTA] Writing updater to '%s' @ 0x%x\n",
                millis(), app1->label, app1->address);

  // Write updater firmware to app1 using OTA API
  esp_ota_handle_t handle;
  esp_err_t err = esp_ota_begin(app1, updaterSize, &handle);
  if (err != ESP_OK) {
    Serial.printf("[%lu] [SdOTA] esp_ota_begin failed: %s\n", millis(), esp_err_to_name(err));
    return FLASH_ERROR;
  }

  // Write in chunks for progress reporting
  constexpr size_t chunkSize = 4096;
  size_t written = 0;

  while (written < updaterSize) {
    size_t toWrite = updaterSize - written;
    if (toWrite > chunkSize) toWrite = chunkSize;

    err = esp_ota_write(handle, updater_fw_start + written, toWrite);
    if (err != ESP_OK) {
      Serial.printf("[%lu] [SdOTA] Write failed at %u: %s\n", millis(), written, esp_err_to_name(err));
      esp_ota_abort(handle);
      return FLASH_ERROR;
    }

    written += toWrite;
    if (progress) progress(written, updaterSize);
  }

  err = esp_ota_end(handle);
  if (err != ESP_OK) {
    Serial.printf("[%lu] [SdOTA] esp_ota_end failed: %s\n", millis(), esp_err_to_name(err));
    return FLASH_ERROR;
  }

  // Switch boot to app1
  err = esp_ota_set_boot_partition(app1);
  if (err != ESP_OK) {
    Serial.printf("[%lu] [SdOTA] Set boot partition failed: %s\n", millis(), esp_err_to_name(err));
    return FLASH_ERROR;
  }

  Serial.printf("[%lu] [SdOTA] Updater written. Rebooting to updater...\n", millis());
  return OK;
}
