#include "CrossPointState.h"

#include <HardwareSerial.h>
#include <SDCardManager.h>
#include <Serialization.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {
constexpr uint8_t STATE_FILE_VERSION = 5;
constexpr char STATE_FILE[] = "/.crosspoint/state.bin";

StaticSemaphore_t STATE_FILE_MUTEX_BUFFER;
SemaphoreHandle_t STATE_FILE_MUTEX = nullptr;

SemaphoreHandle_t getStateFileMutex() {
  if (!STATE_FILE_MUTEX) {
    STATE_FILE_MUTEX = xSemaphoreCreateMutexStatic(&STATE_FILE_MUTEX_BUFFER);
  }
  return STATE_FILE_MUTEX;
}
}  // namespace

CrossPointState CrossPointState::instance;

bool CrossPointState::saveToFile() const {
  SemaphoreHandle_t mutex = getStateFileMutex();
  if (mutex) {
    xSemaphoreTake(mutex, portMAX_DELAY);
  }

  const std::string openEpubPathSnapshot = openEpubPath;
  const uint8_t lastSleepImageSnapshot = lastSleepImage;
  const uint8_t readerActivityLoadCountSnapshot = readerActivityLoadCount;
  const bool lastSleepFromReaderSnapshot = lastSleepFromReader;
  const bool isRenderCompleteSnapshot = isRenderComplete;

  bool ok = false;
  FsFile outputFile;
  if (!SdMan.openFileForWrite("CPS", STATE_FILE, outputFile)) {
    if (mutex) {
      xSemaphoreGive(mutex);
    }
    return false;
  }
  Serial.printf("[%lu] [CPS] Saving state to file: openEpubPath='%s', lastSleepImage=%u, readerActivityLoadCount=%u, lastSleepFromReader=%d\n",
                millis(), openEpubPathSnapshot.c_str(), lastSleepImageSnapshot, readerActivityLoadCountSnapshot,
                lastSleepFromReaderSnapshot);

  serialization::writePod(outputFile, STATE_FILE_VERSION);
  serialization::writeString(outputFile, openEpubPathSnapshot);
  serialization::writePod(outputFile, lastSleepImageSnapshot);
  serialization::writePod(outputFile, readerActivityLoadCountSnapshot);
  serialization::writePod(outputFile, lastSleepFromReaderSnapshot);
  serialization::writePod(outputFile, isRenderCompleteSnapshot);
  outputFile.close();
  ok = true;

  if (mutex) {
    xSemaphoreGive(mutex);
  }
  return ok;
}

bool CrossPointState::loadFromFile() {
  SemaphoreHandle_t mutex = getStateFileMutex();
  if (mutex) {
    xSemaphoreTake(mutex, portMAX_DELAY);
  }

  FsFile inputFile;
  if (!SdMan.openFileForRead("CPS", STATE_FILE, inputFile)) {
    if (mutex) {
      xSemaphoreGive(mutex);
    }
    return false;
  }

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version > STATE_FILE_VERSION) {
    Serial.printf("[%lu] [CPS] Deserialization failed: Unknown version %u\n", millis(), version);
    inputFile.close();
    if (mutex) {
      xSemaphoreGive(mutex);
    }
    return false;
  }

  serialization::readString(inputFile, openEpubPath);
  if (version >= 2) {
    serialization::readPod(inputFile, lastSleepImage);
  } else {
    lastSleepImage = 0;
  }

  if (version >= 3) {
    serialization::readPod(inputFile, readerActivityLoadCount);
  }

  if (version >= 4) {
    serialization::readPod(inputFile, lastSleepFromReader);
  } else {
    lastSleepFromReader = false;
  }
  if (version >= 5) {
    serialization::readPod(inputFile, isRenderComplete);
  } else {
    isRenderComplete = false;
  }
  inputFile.close();

  if (mutex) {
    xSemaphoreGive(mutex);
  }
  return true;
}
