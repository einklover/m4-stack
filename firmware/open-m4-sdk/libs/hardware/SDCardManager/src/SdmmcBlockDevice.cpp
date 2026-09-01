#include "SdmmcBlockDevice.h"

#if FREEINK_SD_SDMMC

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

namespace freeink {

void SdmmcBlockDevice::setErr(SdmmcFailCode code, SdmmcFailStage stage, int attempt, int espErr,
                              const char* detail) {
  _lastError.code = code;
  _lastError.stage = stage;
  _lastError.attempt = attempt;
  _lastError.espErr = espErr;
  if (detail) {
    size_t i = 0;
    for (; detail[i] && i + 1 < sizeof(_lastError.detail); ++i) _lastError.detail[i] = detail[i];
    _lastError.detail[i] = 0;
  } else {
    _lastError.detail[0] = 0;
  }
}

bool SdmmcBlockDevice::begin(const BoardConfig::SdmmcPins& pins) {
  _lastError = {};
  if (pins.busWidth == 0) {
    setErr(SdmmcFailCode::HostInitFailed, SdmmcFailStage::HostInit, 0, -1, "busWidth=0");
    return false;
  }

  // Idempotent re-enter: a prior failed/partial begin must not leave the host up.
  if (_card) end();

  // Host config: start at DEFAULT (40 MHz). On retries we drop to PROBING (400 kHz)
  // so marginal cards / long lines still complete card_init (often misreported as
  // "no_card" when the bus was simply too fast at power-up).
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.max_freq_khz = SDMMC_FREQ_DEFAULT;  // 40 MHz first attempt

  // Slot pin map. The ESP32-S3 routes SDMMC through the GPIO matrix, so the data
  // and clock/command lines are assignable (unlike the classic ESP32's fixed slot).
  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = pins.busWidth;
  slot.clk = static_cast<gpio_num_t>(pins.clk);
  slot.cmd = static_cast<gpio_num_t>(pins.cmd);
  slot.d0 = static_cast<gpio_num_t>(pins.d0);
  slot.d1 = static_cast<gpio_num_t>(pins.d1);
  slot.d2 = static_cast<gpio_num_t>(pins.d2);
  slot.d3 = static_cast<gpio_num_t>(pins.d3);
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  esp_err_t he = sdmmc_host_init();
  if (he != ESP_OK) {
    setErr(SdmmcFailCode::HostInitFailed, SdmmcFailStage::HostInit, 0, static_cast<int>(he), "sdmmc_host_init");
    return false;
  }
  he = sdmmc_host_init_slot(host.slot, &slot);
  if (he != ESP_OK) {
    setErr(SdmmcFailCode::SlotInitFailed, SdmmcFailStage::SlotInit, 0, static_cast<int>(he), "sdmmc_host_init_slot");
    sdmmc_host_deinit();
    return false;
  }

  // INTERNAL_PULLUP often does NOT engage on ESP32-S3 matrix pins. Force pull-ups
  // on CMD + all used DAT lines after the driver claims them (floating DAT →
  // card_init fails with no CSD → surfaces as no_card even when a card is present).
  gpio_pullup_en(static_cast<gpio_num_t>(pins.cmd));
  gpio_pullup_en(static_cast<gpio_num_t>(pins.d0));
  if (pins.busWidth >= 4) {
    if (pins.d1 >= 0) gpio_pullup_en(static_cast<gpio_num_t>(pins.d1));
    if (pins.d2 >= 0) gpio_pullup_en(static_cast<gpio_num_t>(pins.d2));
    if (pins.d3 >= 0) gpio_pullup_en(static_cast<gpio_num_t>(pins.d3));
  }

  auto* card = static_cast<sdmmc_card_t*>(malloc(sizeof(sdmmc_card_t)));
  if (!card) {
    setErr(SdmmcFailCode::Oom, SdmmcFailStage::CardInit, 0, -1, "card alloc");
    sdmmc_host_deinit();
    return false;
  }
  // SD power/enable (e.g. Murphy M4 GPIO10 active-low, X4 Pro GPIO5 active-low).
  // Respect BoardConfig::sd.powerActiveHigh: ON level powers the card; OFF cuts it.
  const int8_t sdPwr = BoardConfig::ACTIVE.sd.powerEnable;
  const bool pwrActiveHigh = BoardConfig::ACTIVE.sd.powerActiveHigh;
  const int pwrOn = pwrActiveHigh ? HIGH : LOW;
  const int pwrOff = pwrActiveHigh ? LOW : HIGH;
  if (sdPwr >= 0) {
    gpio_hold_dis(static_cast<gpio_num_t>(sdPwr));
    pinMode(sdPwr, OUTPUT);
  }
  // DMA-capable bounce buffer for the sector-0 validation read. SdFat's own cache may
  // live in PSRAM / at an unaligned address; a known-good internal-RAM buffer proves
  // the data path on its own terms.
  auto* probe = static_cast<uint8_t*>(heap_caps_malloc(512, MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
  if (!probe) {
    setErr(SdmmcFailCode::Oom, SdmmcFailStage::SectorRead, 0, -1, "probe alloc");
    free(card);
    sdmmc_host_deinit();
    return false;
  }

  // Retry full mount (card_init + sector-0 read). Power-cycle before each attempt.
  // Later attempts drop host max_freq to PROBING so slow/noisy buses still init.
  constexpr int kMaxAttempts = 6;
  esp_err_t mountErr = ESP_FAIL;
  SdmmcFailCode failCode = SdmmcFailCode::NoCard;
  SdmmcFailStage failStage = SdmmcFailStage::CardInit;
  int lastAttempt = 0;  // 0-based; log as attempt+1 for humans
  for (int attempt = 0; attempt < kMaxAttempts; attempt++) {
    lastAttempt = attempt;
    // Odd attempts: 400 kHz probing clock (more reliable right after power-up).
    host.max_freq_khz = (attempt == 0) ? SDMMC_FREQ_DEFAULT : SDMMC_FREQ_PROBING;
    if (sdPwr >= 0) {
      digitalWrite(sdPwr, pwrOff);
      delay(attempt == 0 ? 50 : 100);  // ensure rail is off
      digitalWrite(sdPwr, pwrOn);
      // Longer settle for cold boot / after flash reset (card may need >100ms).
      delay(attempt < 2 ? 150 : 220);
    } else {
      delay(20);
    }
    // Zero card so stale csd.capacity from a prior attempt cannot misclassify
    // a failed card_init as "CSD known → sector path".
    memset(card, 0, sizeof(sdmmc_card_t));
    esp_err_t e = sdmmc_card_init(&host, card);
    if (e != ESP_OK && card->csd.capacity == 0) {
      mountErr = e;  // failed before CSD — nothing to read; power-cycle and retry
      failCode = SdmmcFailCode::NoCard;
      failStage = SdmmcFailStage::CardInit;
      if (Serial)
        Serial.printf("[%lu] [SD] card_init fail attempt=%d/%d err=%s freq=%d\n", millis(), attempt + 1,
                      kMaxAttempts, esp_err_to_name(e), host.max_freq_khz);
      continue;
    }
    // CSD is valid (capacity known); prove real block I/O before committing.
    e = sdmmc_read_sectors(card, probe, 0, 1);
    mountErr = e;
    if (e == ESP_OK) break;
    // Classify sector errors: ESP_ERR_TIMEOUT and common 0x107 paths → timeout.
    failStage = SdmmcFailStage::SectorRead;
    if (e == ESP_ERR_TIMEOUT || e == 0x107) {
      failCode = SdmmcFailCode::SectorTimeout;
    } else {
      failCode = SdmmcFailCode::SectorIoError;
    }
    if (Serial)
      Serial.printf("[%lu] [SD] sector0 fail attempt=%d/%d err=%s\n", millis(), attempt + 1, kMaxAttempts,
                    esp_err_to_name(e));
  }
  heap_caps_free(probe);

  if (mountErr != ESP_OK) {
    setErr(failCode, failStage, lastAttempt, static_cast<int>(mountErr), esp_err_to_name(mountErr));
    if (Serial)
      Serial.printf("[%lu] [SD] SDMMC mount failed after retries: stage=%d code=%d attempt=%d/%d %s\n", millis(),
                    static_cast<int>(failStage), static_cast<int>(failCode), lastAttempt + 1, kMaxAttempts,
                    esp_err_to_name(mountErr));
    free(card);
    sdmmc_host_deinit();
    return false;
  }
  // Prefer higher clock after a probing-frequency success (optional; keep if set high).
  if (host.max_freq_khz < SDMMC_FREQ_DEFAULT) {
    // Best-effort: leave card at successful freq; changing after init is not always supported.
  }
  _card = card;
  setErr(SdmmcFailCode::Ok, SdmmcFailStage::None, lastAttempt, 0, "ok");
  if (Serial)
    Serial.printf("[%lu] [SD] SDMMC ok attempt=%d/%d sectors=%llu freq_khz=%d\n", millis(), lastAttempt + 1,
                  kMaxAttempts, static_cast<unsigned long long>(card->csd.capacity), host.max_freq_khz);
  return true;
}

void SdmmcBlockDevice::end() {
  if (_card) {
    free(_card);
    _card = nullptr;
    sdmmc_host_deinit();
  }
}

// esp-idf SDMMC uses DMA, which requires the transfer buffer to be in DMA-capable
// internal RAM and word-aligned. SdFat's cache buffers aren't guaranteed to be
// (PSRAM / arbitrary alignment), which makes sdmmc_read/write_sectors fail. Bounce
// through a DMA-capable buffer. (heap_caps_aligned_alloc via MALLOC_CAP_DMA.)
bool SdmmcBlockDevice::readSectors(Sector_t sector, uint8_t* dst, size_t ns) {
  if (!_card) return false;
  const size_t bytes = ns * 512u;
  auto* bounce = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
  if (!bounce) return false;
  const esp_err_t e = sdmmc_read_sectors(static_cast<sdmmc_card_t*>(_card), bounce, sector, ns);
  if (e == ESP_OK) memcpy(dst, bounce, bytes);
  heap_caps_free(bounce);
  return e == ESP_OK;
}

bool SdmmcBlockDevice::writeSectors(Sector_t sector, const uint8_t* src, size_t ns) {
  if (!_card) return false;
  const size_t bytes = ns * 512u;
  auto* bounce = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
  if (!bounce) return false;
  memcpy(bounce, src, bytes);
  const esp_err_t e = sdmmc_write_sectors(static_cast<sdmmc_card_t*>(_card), bounce, sector, ns);
  heap_caps_free(bounce);
  return e == ESP_OK;
}

Sector_t SdmmcBlockDevice::sectorCount() {
  return _card ? static_cast<Sector_t>(static_cast<sdmmc_card_t*>(_card)->csd.capacity) : 0;
}

}  // namespace freeink

#endif  // FREEINK_SD_SDMMC
