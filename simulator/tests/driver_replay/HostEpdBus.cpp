#include "HostEpdBridge.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "core/SimKernel.h"
#include "hardware/SimSpiBus.h"
#include "hardware/SimSsd1677Controller.h"
#include "bus/EpdBus.h"

namespace {
hostreplay::Context* gContext = nullptr;

hostreplay::Context& ctx() {
  if (!gContext) throw "host EpdBus replay context not bound";
  return *gContext;
}

bool beginFrame() {
  auto& c = ctx();
  if (c.transactionOpen) return true;
  const bool ok = c.spi && c.spi->begin(c.cs, c.hz);
  c.protocolOk = c.protocolOk && ok;
  return ok;
}

bool endFrame() {
  auto& c = ctx();
  if (c.transactionOpen) return true;
  const bool ok = c.spi && c.spi->end();
  c.protocolOk = c.protocolOk && ok;
  return ok;
}

void waitControllerIdle() {
  auto& c = ctx();
  uint32_t guard = 0;
  while (c.epd && c.epd->busy() && guard++ < 10000) {
    if (c.scheduler) c.scheduler->runFor(1);
    else break;
  }
  if (c.epd && c.epd->busy()) c.protocolOk = false;
}
}

namespace hostreplay {
void bind(Context* context) { gContext = context; }
Context* context() { return gContext; }
}  // namespace hostreplay

unsigned long millis() {
  return gContext && gContext->scheduler ? gContext->scheduler->now() : 0;
}
void delay(unsigned long ms) {
  if (gContext && gContext->scheduler) gContext->scheduler->runFor(static_cast<uint32_t>(ms));
}
int digitalRead(int) {
  return gContext && gContext->epd && gContext->epd->busy() ? HIGH : LOW;
}
void digitalWrite(int, int) {}
void pinMode(int, int) {}

namespace freeink {

void EpdBus::begin(const EpdPins& pins, uint32_t spiHz, BusyPolarity busy, int8_t, int8_t coCs) {
  _pins = pins;
  _spiHz = spiHz;
  _busy = busy;
  _coCs = coCs;
  auto& c = ctx();
  c.cs = pins.cs;
  c.hz = spiHz;
}

void EpdBus::reset(uint16_t extraSettleMs) {
  auto& c = ctx();
  if (c.epd) c.epd->hardReset(/*preservePhysical=*/true);
  if (c.scheduler) c.scheduler->runFor(static_cast<uint32_t>(20 + extraSettleMs));
}

void EpdBus::cmd(uint8_t c) {
  if (!beginFrame()) return;
  auto& h = ctx();
  h.protocolOk = h.protocolOk && h.spi->writeCommand(c);
  endFrame();
}

void EpdBus::data(uint8_t d) {
  if (!beginFrame()) return;
  auto& h = ctx();
  h.protocolOk = h.protocolOk && h.spi->writeData(d);
  endFrame();
}

void EpdBus::data(const uint8_t* d, uint16_t len) {
  if (!beginFrame()) return;
  auto& h = ctx();
  h.protocolOk = h.protocolOk && h.spi->writeData(d, len);
  endFrame();
}

void EpdBus::cmdData(uint8_t c, const uint8_t* d, uint16_t len) {
  beginTxn();
  rawCmd(c);
  rawWriteBytes(d, len);
  endTxn();
}

void EpdBus::cmdData2(uint8_t c, uint8_t d0, uint8_t d1) {
  const uint8_t dataBytes[2] = {d0, d1};
  cmdData(c, dataBytes, 2);
}

void EpdBus::beginTxn() {
  auto& c = ctx();
  if (c.transactionOpen) {
    c.protocolOk = false;
    return;
  }
  c.protocolOk = c.protocolOk && c.spi && c.spi->begin(c.cs, c.hz);
  c.transactionOpen = true;
}

void EpdBus::endTxn() {
  auto& c = ctx();
  if (!c.transactionOpen) {
    c.protocolOk = false;
    return;
  }
  c.protocolOk = c.protocolOk && c.spi && c.spi->end();
  c.transactionOpen = false;
}

void EpdBus::rawCmd(uint8_t c) {
  auto& h = ctx();
  if (!h.transactionOpen || !h.spi) {
    h.protocolOk = false;
    return;
  }
  h.protocolOk = h.protocolOk && h.spi->writeCommand(c);
}

void EpdBus::rawData(uint8_t d) {
  auto& h = ctx();
  if (!h.transactionOpen || !h.spi) {
    h.protocolOk = false;
    return;
  }
  h.protocolOk = h.protocolOk && h.spi->writeData(d);
}

void EpdBus::rawWriteBytes(const uint8_t* d, uint16_t len) {
  auto& h = ctx();
  if (!h.transactionOpen || !h.spi) {
    h.protocolOk = false;
    return;
  }
  h.protocolOk = h.protocolOk && h.spi->writeData(d, len);
}

void EpdBus::waitBusy(const char*) { waitControllerIdle(); }
void EpdBus::waitBusy(BusyPolarity, const char*) { waitControllerIdle(); }
void EpdBus::waitRefreshComplete(const char*) { waitControllerIdle(); }

void EpdBus::writeMirroredPlane(const uint8_t* plane, uint16_t height,
                                uint16_t widthBytes, bool invert) {
  std::vector<uint8_t> row(widthBytes);
  for (int y = static_cast<int>(height) - 1; y >= 0; --y) {
    const uint8_t* src = plane + static_cast<size_t>(y) * widthBytes;
    if (invert) {
      std::transform(src, src + widthBytes, row.begin(), [](uint8_t b) {
        return static_cast<uint8_t>(~b);
      });
      data(row.data(), widthBytes);
    } else {
      data(src, widthBytes);
    }
  }
}

void EpdBus::sendPlaneFlipped(uint8_t ramCmd, const uint8_t* plane,
                              uint16_t height, uint16_t widthBytes) {
  cmd(ramCmd);
  beginTxn();
  for (int y = static_cast<int>(height) - 1; y >= 0; --y) {
    rawWriteBytes(plane + static_cast<size_t>(y) * widthBytes, widthBytes);
  }
  endTxn();
}

void EpdBus::fillPlane(uint8_t ramCmd, uint8_t fillByte, uint16_t height,
                       uint16_t widthBytes) {
  cmd(ramCmd);
  std::vector<uint8_t> row(widthBytes, fillByte);
  beginTxn();
  for (uint16_t y = 0; y < height; ++y) rawWriteBytes(row.data(), widthBytes);
  endTxn();
}

}  // namespace freeink
