#pragma once

#include <cstdint>

namespace m4sim {
class SimScheduler;
class SimSpiBus;
class SimSsd1677Controller;
}

namespace hostreplay {

struct Context {
  m4sim::SimScheduler* scheduler = nullptr;
  m4sim::SimSpiBus* spi = nullptr;
  m4sim::SimSsd1677Controller* epd = nullptr;
  int cs = -1;
  uint32_t hz = 0;
  bool transactionOpen = false;
  bool protocolOk = true;
};

void bind(Context* context);
Context* context();

}  // namespace hostreplay
