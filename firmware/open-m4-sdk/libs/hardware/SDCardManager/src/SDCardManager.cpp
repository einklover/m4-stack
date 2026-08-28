#include "SDCardManager.h"

#include <BoardConfig.h>
#include <driver/gpio.h>
#include <SPI.h>

#include "SdmmcBlockDevice.h"  // no-op unless FREEINK_SD_SDMMC
// Production mapping seam (must match freeink::SdmmcFailCode order).
#include "../../../../../src/util/M4SdStatus.h"

#if FREEINK_SD_SDMMC
// Drift guard: freeink::SdmmcFailCode ordinals must match M4SdStatus::BlockCode.
static_assert(static_cast<int>(freeink::SdmmcFailCode::Ok) == static_cast<int>(M4SdStatus::BlockCode::Ok),
              "SdmmcFailCode/BlockCode Ok ordinal mismatch");
static_assert(static_cast<int>(freeink::SdmmcFailCode::HostInitFailed) ==
                  static_cast<int>(M4SdStatus::BlockCode::HostInitFailed),
              "SdmmcFailCode/BlockCode HostInitFailed ordinal mismatch");
static_assert(static_cast<int>(freeink::SdmmcFailCode::SlotInitFailed) ==
                  static_cast<int>(M4SdStatus::BlockCode::SlotInitFailed),
              "SdmmcFailCode/BlockCode SlotInitFailed ordinal mismatch");
static_assert(static_cast<int>(freeink::SdmmcFailCode::NoCard) == static_cast<int>(M4SdStatus::BlockCode::NoCard),
              "SdmmcFailCode/BlockCode NoCard ordinal mismatch");
static_assert(static_cast<int>(freeink::SdmmcFailCode::SectorTimeout) ==
                  static_cast<int>(M4SdStatus::BlockCode::SectorTimeout),
              "SdmmcFailCode/BlockCode SectorTimeout ordinal mismatch");
static_assert(static_cast<int>(freeink::SdmmcFailCode::SectorIoError) ==
                  static_cast<int>(M4SdStatus::BlockCode::SectorIoError),
              "SdmmcFailCode/BlockCode SectorIoError ordinal mismatch");
static_assert(static_cast<int>(freeink::SdmmcFailCode::Oom) == static_cast<int>(M4SdStatus::BlockCode::Oom),
              "SdmmcFailCode/BlockCode Oom ordinal mismatch");
#endif

SDCardManager SDCardManager::instance;

#if FREEINK_SD_SDMMC
SDCardManager::SDCardManager() {}

bool SDCardManager::begin() {
  // Native SDMMC: SdFat can't drive SDIO, so mount a plain FsVolume on the esp-idf
  // SDMMC block device. FsFile from this volume is the same type the SPI path
  // returns, so the public API and consumers are unchanged.
  setLast("power_cycle", "begin", "not_initialized");
  if (_powerHook) _powerHook();  // board brings up its SD rail (e.g. PMIC) if needed
  // The SD power-enable (sd.powerEnable) is driven by SdmmcBlockDevice itself, which
  // reproduces the OEM's timed HIGH->LOW power-cycle around each mount attempt — do
  // NOT assert it here (holding it HIGH going in breaks that reset sequence).
  if (!_dev) _dev = new freeink::SdmmcBlockDevice();
  if (!_dev->begin(BoardConfig::ACTIVE.sdmmc)) {
    // Exact production mapper (host tests call the same function).
    const auto& le = _dev->lastError();
    const auto mapped =
        M4SdStatus::mapSdmmcFailToReport(static_cast<int>(le.code), static_cast<int>(le.stage));
    setLast(mapped.stage, le.detail[0] ? le.detail : "SDMMC init failed", mapped.code);
    if (Serial)
      Serial.printf("[%lu] [SD] SDMMC init failed stage=%s code=%s detail=%s attempt=%d\n", millis(), mapped.stage,
                    mapped.code, _lastDetail, le.attempt + 1);  // human 1-based attempt
    initialized = false;
    cachedTotalBytes = 0;
    cachedUsedBytesValid = false;
    return false;
  }
  _lastSectorCount = static_cast<uint64_t>(_dev->sectorCount());
  // SdFat defaults to MBR partition 1. ESP-IDF's FATFS (and therefore some OEM
  // firmware/card-formatting tools) also accepts a FAT/exFAT volume written
  // directly at sector 0 ("superfloppy"), and cards may place the usable volume
  // in another primary MBR slot. Probe all four primary slots first, then sector
  // zero. These are read-only mount probes; no format or partition write occurs.
  setLast("volume_mount", "probing MBR 1-4 then superfloppy", "ok");
  int mountedPart = -1;
  for (uint8_t part = 1; part <= 4; ++part) {
    if (_vol.begin(_dev, true, part)) {
      mountedPart = part;
      break;
    }
  }
  if (mountedPart < 0 && _vol.begin(_dev, true, 0)) mountedPart = 0;

  if (mountedPart < 0) {
    setLast("volume_mount", "unsupported or missing FAT volume", "unsupported_fs");
    if (Serial) Serial.printf("[%lu] [SD] SDMMC volume mount failed (unsupported_fs)\n", millis());
    _dev->end();
    initialized = false;
    cachedTotalBytes = 0;
    cachedUsedBytesValid = false;
    return false;
  }
  _lastMountedPart = mountedPart;
  _lastFatType = static_cast<uint8_t>(_vol.fatType());
  if (Serial)
    Serial.printf("[%lu] [SD] SDMMC card mounted part=%d fatType=%u sectors=%llu\n", millis(), mountedPart,
                  static_cast<unsigned>(_vol.fatType()), static_cast<unsigned long long>(_dev->sectorCount()));
  initialized = true;
  cachedTotalBytes = static_cast<uint64_t>(vol().clusterCount()) * vol().bytesPerCluster();
  cachedUsedBytesValid = false;
  setLast("ready", "mounted", "ok");
  return initialized;
}
#else
SDCardManager::SDCardManager() : sd() {}

bool SDCardManager::begin() {
  // Profiles whose SD CS is not yet known leave it unassigned so the card stays
  // dormant — bail out before any pin is touched, or SdFat drives "pin 255" and
  // floods the log. (Native-SDMMC boards like the X4 Pro take the #if branch above.)
  if (BoardConfig::ACTIVE.sd.cs < 0) {
    if (Serial) Serial.printf("[%lu] [SD] SD disabled: CS unassigned in the %s profile\n", millis(), BoardConfig::ACTIVE.name);
    initialized = false;
    cachedTotalBytes = 0;
    cachedUsedBytesValid = false;
    return false;
  }

  // Pins/clock come from the runtime-active profile (board-overridable via
  // BoardConfig::ACTIVE.sd.spiHz; 0 = default). Read after device selection.
  const uint8_t SD_CS = BoardConfig::ACTIVE.sd.cs;
  const int8_t SD_SCLK = BoardConfig::ACTIVE.sd.sclk >= 0 ? BoardConfig::ACTIVE.sd.sclk
                                                          : (BoardConfig::ACTIVE.sd.separateSpi ? -1
                                                                                                : BoardConfig::ACTIVE.display.sclk);
  const int8_t SD_MOSI = BoardConfig::ACTIVE.sd.mosi >= 0 ? BoardConfig::ACTIVE.sd.mosi
                                                          : (BoardConfig::ACTIVE.sd.separateSpi ? -1
                                                                                                : BoardConfig::ACTIVE.display.mosi);
  const int8_t SD_MISO = BoardConfig::ACTIVE.sd.miso;
  const uint32_t SPI_FQ = BoardConfig::ACTIVE.sd.spiHz != 0 ? BoardConfig::ACTIVE.sd.spiHz : 40000000;

  if (_powerHook) _powerHook();  // board brings up its SD rail (e.g. PMIC) if needed

  // Boards that gate the SD rail with a plain GPIO (e.g. Sticky's SD_PWR_EN on
  // GPIO10) must power it before probing. Active-high enable + a brief settle.
  // No-op when unassigned, and complements _powerHook for PMIC-gated boards.
  // gpio_hold_dis first: the sleep path holds this pin LOW and the hold survives
  // the deep-sleep wake reset; the HIGH write is a no-op until it is released.
  if (BoardConfig::ACTIVE.sd.powerEnable >= 0) {
    gpio_hold_dis(static_cast<gpio_num_t>(BoardConfig::ACTIVE.sd.powerEnable));
    pinMode(BoardConfig::ACTIVE.sd.powerEnable, OUTPUT);
    // ON level: HIGH for active-high enables, LOW for active-low ones.
    digitalWrite(BoardConfig::ACTIVE.sd.powerEnable, BoardConfig::ACTIVE.sd.powerActiveHigh ? HIGH : LOW);
    delay(10);
  }

  // Shared SPI bus: when the display controller sits on the same SCLK as the SD
  // card (e.g. M5Paper's IT8951 on 14/12/13), deselect it (CS high) before
  // probing the card. SD init runs before the display driver's begin(), so a
  // powered, never-deselected panel can drive the shared MISO and break
  // detection. Harmless when the display is on a separate bus or has no CS pin.
  if (BoardConfig::ACTIVE.display.cs >= 0 && BoardConfig::ACTIVE.display.sclk == SD_SCLK) {
    pinMode(BoardConfig::ACTIVE.display.cs, OUTPUT);
    digitalWrite(BoardConfig::ACTIVE.display.cs, HIGH);
  }

  if (SD_SCLK >= 0 && SD_MOSI >= 0 && SD_MISO >= 0) {
    SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  }

  if (!sd.begin(SD_CS, SPI_FQ)) {
    if (Serial)
      Serial.printf("[%lu] [SD] SD card not detected (err=0x%02X data=0x%02X cs=%d sclk=%d miso=%d mosi=%d clk=%luHz)\n",
                    millis(), sd.sdErrorCode(), sd.sdErrorData(), SD_CS, SD_SCLK,
                    SD_MISO, SD_MOSI, (unsigned long)SPI_FQ);
    initialized = false;
    cachedTotalBytes = 0;
    cachedUsedBytesValid = false;
  } else {
    if (Serial) Serial.printf("[%lu] [SD] SD card detected\n", millis());
    initialized = true;
    cachedTotalBytes = static_cast<uint64_t>(vol().clusterCount()) * vol().bytesPerCluster();
    cachedUsedBytesValid = false;
  }

  return initialized;
}
#endif

bool SDCardManager::capabilityProbe(const char* optionalExistingPath) {
  if (!initialized) {
    setLast("capability_probe", "not initialized", "not_initialized");
    return false;
  }
  setLast("capability_probe", "list root", "ok");
  FsFile root = vol().open("/");
  if (!root || !root.isDirectory()) {
    setLast("capability_probe", "root list failed", "io_failure");
    if (root) root.close();
    return false;
  }
  FsFile entry;
  if (entry.openNext(&root, O_RDONLY)) {
    entry.close();
  }
  root.close();

  if (optionalExistingPath && optionalExistingPath[0] && vol().exists(optionalExistingPath)) {
    FsFile f;
    if (!openFileForRead("SDProbe", optionalExistingPath, f)) {
      setLast("capability_probe", "open existing file failed", "io_failure");
      return false;
    }
    uint8_t buf[16];
    const int n = f.read(buf, sizeof(buf));
    if (n < 0) {
      f.close();
      setLast("capability_probe", "read existing file failed", "io_failure");
      return false;
    }
    if (!f.seekSet(0)) {
      f.close();
      setLast("capability_probe", "seek existing file failed", "io_failure");
      return false;
    }
    f.close();
  }
  setLast("ready", "capability probe ok", "ok");
  return true;
}

bool SDCardManager::ready() const {
  return initialized;
}

std::vector<String> SDCardManager::listFiles(const char* path, const int maxFiles) {
  std::vector<String> ret;
  if (!initialized) {
    if (Serial) Serial.printf("[%lu] [SD] not initialized, returning empty list\n", millis());
    return ret;
  }

  auto root = vol().open(path);
  if (!root) {
    if (Serial) Serial.printf("[%lu] [SD] Failed to open directory\n", millis());
    return ret;
  }
  if (!root.isDirectory()) {
    if (Serial) Serial.printf("[%lu] [SD] Path is not a directory\n", millis());
    root.close();
    return ret;
  }

  int count = 0;
  char name[128];
  for (auto f = root.openNextFile(); f && count < maxFiles; f = root.openNextFile()) {
    if (f.isDirectory()) {
      f.close();
      continue;
    }
    f.getName(name, sizeof(name));
    ret.emplace_back(name);
    f.close();
    count++;
  }
  root.close();
  return ret;
}

String SDCardManager::readFile(const char* path) {
  if (!initialized) {
    if (Serial) Serial.printf("[%lu] [SD] not initialized; cannot read file\n", millis());
    return {""};
  }

  FsFile f;
  if (!openFileForRead("SD", path, f)) {
    return {""};
  }

  String content = "";
  constexpr size_t maxSize = 50000;  // Limit to 50KB
  size_t readSize = 0;
  while (f.available() && readSize < maxSize) {
    const char c = static_cast<char>(f.read());
    content += c;
    readSize++;
  }
  f.close();
  return content;
}

bool SDCardManager::readFileToStream(const char* path, Print& out, const size_t chunkSize) {
  if (!initialized) {
    if (Serial) Serial.println("SDCardManager: not initialized; cannot read file");
    return false;
  }

  FsFile f;
  if (!openFileForRead("SD", path, f)) {
    return false;
  }

  constexpr size_t localBufSize = 256;
  uint8_t buf[localBufSize];
  const size_t toRead = (chunkSize == 0) ? localBufSize : (chunkSize < localBufSize ? chunkSize : localBufSize);

  while (f.available()) {
    const int r = f.read(buf, toRead);
    if (r > 0) {
      out.write(buf, static_cast<size_t>(r));
    } else {
      break;
    }
  }

  f.close();
  return true;
}

size_t SDCardManager::readFileToBuffer(const char* path, char* buffer, const size_t bufferSize, const size_t maxBytes) {
  if (!buffer || bufferSize == 0)
    return 0;
  if (!initialized) {
    if (Serial) Serial.println("SDCardManager: not initialized; cannot read file");
    buffer[0] = '\0';
    return 0;
  }

  FsFile f;
  if (!openFileForRead("SD", path, f)) {
    buffer[0] = '\0';
    return 0;
  }

  const size_t maxToRead = (maxBytes == 0) ? (bufferSize - 1) : min(maxBytes, bufferSize - 1);
  size_t total = 0;

  while (f.available() && total < maxToRead) {
    constexpr size_t chunk = 64;
    const size_t want = maxToRead - total;
    const size_t readLen = (want < chunk) ? want : chunk;
    const int r = f.read(buffer + total, readLen);
    if (r > 0) {
      total += static_cast<size_t>(r);
    } else {
      break;
    }
  }

  buffer[total] = '\0';
  f.close();
  return total;
}

bool SDCardManager::writeFile(const char* path, const String& content) {
  if (!initialized) {
    if (Serial) Serial.println("SDCardManager: not initialized; cannot write file");
    return false;
  }

  if (vol().exists(path)) {
    vol().remove(path);
  }

  FsFile f;
  if (!openFileForWrite("SD", path, f)) {
    if (Serial) Serial.printf("Failed to open file for write: %s\n", path);
    return false;
  }

  const size_t written = f.print(content);
  f.close();
  return written == content.length();
}

bool SDCardManager::ensureDirectoryExists(const char* path) {
  if (!initialized) {
    if (Serial) Serial.println("SDCardManager: not initialized; cannot create directory");
    return false;
  }

  if (vol().exists(path)) {
    FsFile dir = vol().open(path);
    if (dir && dir.isDirectory()) {
      dir.close();
      return true;
    }
    dir.close();
  }

  if (vol().mkdir(path)) {
    return true;
  }
  if (Serial) Serial.printf("Failed to create directory: %s\n", path);
  return false;
}

bool SDCardManager::openFileForRead(const char* moduleName, const char* path, FsFile& file) {
  if (!vol().exists(path)) {
    if (Serial) Serial.printf("[%lu] [%s] File does not exist: %s\n", millis(), moduleName, path);
    return false;
  }

  file = vol().open(path, O_RDONLY);
  if (!file) {
    if (Serial) Serial.printf("[%lu] [%s] Failed to open file for reading: %s\n", millis(), moduleName, path);
    return false;
  }
  return true;
}

bool SDCardManager::openFileForRead(const char* moduleName, const std::string& path, FsFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool SDCardManager::openFileForRead(const char* moduleName, const String& path, FsFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool SDCardManager::openFileForWrite(const char* moduleName, const char* path, FsFile& file) {
  file = vol().open(path, O_RDWR | O_CREAT | O_TRUNC);
  if (!file) {
    if (Serial) Serial.printf("[%lu] [%s] Failed to open file for writing: %s\n", millis(), moduleName, path);
    return false;
  }
  return true;
}

bool SDCardManager::openFileForWrite(const char* moduleName, const std::string& path, FsFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool SDCardManager::openFileForWrite(const char* moduleName, const String& path, FsFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

uint64_t SDCardManager::sdTotalBytes() const { return cachedTotalBytes; }

uint64_t SDCardManager::sdUsedBytes() {
  if (!initialized) return 0;
  const uint32_t now = millis();
  if (!cachedUsedBytesValid || (now - cachedUsedBytesAt) >= USED_BYTES_CACHE_TTL_MS) {
    const int32_t freeClusters = vol().freeClusterCount();
    const uint64_t clusterCount = vol().clusterCount();
    if (freeClusters < 0) {
      cachedUsedBytes = 0;
    } else {
      const uint64_t cappedFree = (static_cast<uint64_t>(freeClusters) > clusterCount)
                                      ? clusterCount
                                      : static_cast<uint64_t>(freeClusters);
      cachedUsedBytes = (clusterCount - cappedFree) * vol().bytesPerCluster();
    }
    cachedUsedBytesValid = true;
    cachedUsedBytesAt = now;
  }
  return cachedUsedBytes;
}

bool SDCardManager::removeDir(const char* path) {
  auto dir = vol().open(path);
  if (!dir) {
    return false;
  }
  if (!dir.isDirectory()) {
    return false;
  }

  auto file = dir.openNextFile();
  char name[128];
  while (file) {
    String filePath = path;
    if (!filePath.endsWith("/")) {
      filePath += "/";
    }
    file.getName(name, sizeof(name));
    filePath += name;

    if (file.isDirectory()) {
      if (!removeDir(filePath.c_str())) {
        return false;
      }
    } else {
      if (!vol().remove(filePath.c_str())) {
        return false;
      }
    }
    file = dir.openNextFile();
  }

  return vol().rmdir(path);
}
