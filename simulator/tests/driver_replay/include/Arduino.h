#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>

#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define INPUT_PULLDOWN 3
#define OUTPUT_OPEN_DRAIN 4
#define PROGMEM

inline uint8_t pgm_read_byte(const void* p) {
  return *static_cast<const uint8_t*>(p);
}

// BoardConfig only needs the Arduino Serial singleton to exist as a reference
// target in this replay TU; the SSD1677 path does not use any serial methods.
struct HardwareSerial {};
inline HardwareSerial Serial;

unsigned long millis();
void delay(unsigned long ms);
int digitalRead(int pin);
void digitalWrite(int pin, int level);
void pinMode(int pin, int mode);
