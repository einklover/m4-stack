#pragma once

#include <cstdint>

#define MSBFIRST 1
#define SPI_MODE0 0

class SPISettings {
 public:
  SPISettings() = default;
  SPISettings(uint32_t, uint8_t, uint8_t) {}
};
