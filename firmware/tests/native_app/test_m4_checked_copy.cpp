#include "../../src/util/M4CheckedCopy.h"
#include <cassert>
#include <cstring>
#include <vector>
struct Source {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(10000, 42);
  size_t at = 0; int failAt = -1;
  int read(void* p, size_t n) {
    if (failAt >= 0 && at >= static_cast<size_t>(failAt)) return -1;
    n = std::min(n, bytes.size() - at); memcpy(p, bytes.data() + at, n); at += n; return n;
  }
};
struct Dest {
  std::vector<uint8_t> bytes; bool shortWrite=false, syncOk=true, synced=false;
  size_t write(const uint8_t* p, size_t n) { if(shortWrite) --n; bytes.insert(bytes.end(),p,p+n); return n; }
  bool sync() { synced=true; return syncOk; }
};
int main() {
  Source s; Dest d; int yields=0;
  assert(M4CheckedCopy::copy(s,d,10000,[&]{++yields;})); assert(d.bytes==s.bytes && yields==10);
  for(int fault=0;fault<4;++fault) {
    Source a; Dest b;
    if(fault==0) a.failAt=1024;
    if(fault==1) a.bytes.resize(100);
    if(fault==2) b.shortWrite=true;
    if(fault==3) b.syncOk=false;
    assert(!M4CheckedCopy::copy(a,b,10000,[]{}));
  }
  Source empty; Dest target; assert(M4CheckedCopy::copy(empty,target,0,[]{})); assert(target.synced);
}
