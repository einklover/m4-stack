#include <cstddef>
#include <cstdint>
#include <cassert>
static uint32_t clockMs;
uint32_t millis() { return clockMs; }
#include "../../src/network/M4DeadlineClient.h"
struct Client {
  bool live=true; int remaining=0;
  virtual int available() { return remaining; }
  virtual uint8_t connected() { return live; }
  virtual int read(uint8_t*, size_t n) { if (!remaining) return -1; --remaining; return n ? 1 : 0; }
  virtual int read() { return -1; }
  void stop() { live=false; }
};
int main() {
  M4DeadlineClient<Client> silent; silent.startBody(45000);
  for(clockMs=0;clockMs<15000;++clockMs) assert(silent.connected() && silent.available()==0);
  assert(!silent.connected() && silent.expired()); // zero writes, unknown length
  clockMs=0; M4DeadlineClient<Client> drip; drip.startBody(45000);
  uint8_t b;
  for(clockMs=0;clockMs<45000;clockMs+=10000) { drip.remaining=1; assert(drip.read(&b,1)==1); }
  assert(!drip.connected() && drip.expired());
  clockMs=UINT32_MAX-100; M4DeadlineClient<Client> wrap; wrap.startBody(45000);
  clockMs+=14999; assert(wrap.connected()); ++clockMs; assert(!wrap.connected());
}
