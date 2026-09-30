"""Compile the production TLS allocation hook with explicit capability/failure shims."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as tmp:
    d = pathlib.Path(tmp)
    (d / "mbedtls").mkdir()
    (d / "esp_heap_caps.h").write_text('''#pragma once
#include <cstddef>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
void* heap_caps_calloc(size_t, size_t, unsigned);
void heap_caps_free(void*);
size_t heap_caps_get_total_size(unsigned);
size_t heap_caps_get_free_size(unsigned);
size_t heap_caps_get_largest_free_block(unsigned);
''')
    (d / "mbedtls/platform.h").write_text('''#pragma once
#include <cstddef>
int mbedtls_platform_set_calloc_free(void*(*)(size_t,size_t),void(*)(void*));
''')
    (d / "test.cpp").write_text('''#include <cassert>
#include <cstdlib>
#include <cstring>
#include "util/M4TlsMemory.h"
#include "apps/providers/M4NativeProviderHeavyGate.h"
static size_t total = 1<<20;
static bool fail = false;
static int calls = 0, installs = 0, frees = 0;
static size_t internalFree=96676, internalLargest=18420;
static size_t externalFree=1700000, externalLargest=1000000;
static void* (*allocHook)(size_t,size_t) = nullptr;
static void (*freeHook)(void*) = nullptr;
void* heap_caps_calloc(size_t n,size_t s,unsigned caps) {
  assert(caps == (MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
  ++calls; return fail ? nullptr : calloc(n,s);
}
void heap_caps_free(void* p) { ++frees; free(p); }
size_t heap_caps_get_total_size(unsigned caps) {
  assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)); return total;
}
size_t heap_caps_get_free_size(unsigned caps) {
  return caps & MALLOC_CAP_INTERNAL ? internalFree : externalFree;
}
size_t heap_caps_get_largest_free_block(unsigned caps) {
  return caps & MALLOC_CAP_INTERNAL ? internalLargest : externalLargest;
}
int mbedtls_platform_set_calloc_free(void*(*a)(size_t,size_t),void(*f)(void*)) {
  ++installs; allocHook=a; freeHook=f; return 0;
}
int main() {
  total=0; assert(!M4TlsMemory::install()); assert(installs==0);
  // Exact device failure: the SDK-default internal gate rejects this heap.
  assert(!M4TlsMemory::externalActive());
  assert(!M4NativeProviderHeavyGate::tlsBlockAvailable());
  total=1<<20; assert(M4TlsMemory::install()); assert(installs==1);
  assert(M4TlsMemory::externalActive());
  assert(M4NativeProviderHeavyGate::tlsBlockAvailable());
  internalFree=31000; assert(!M4NativeProviderHeavyGate::tlsBlockAvailable());
  internalFree=96676; internalLargest=7000;
  assert(!M4NativeProviderHeavyGate::tlsBlockAvailable());
  internalLargest=18420; externalFree=100000;
  assert(!M4NativeProviderHeavyGate::tlsBlockAvailable());
  externalFree=1700000; externalLargest=31000;
  assert(!M4NativeProviderHeavyGate::tlsBlockAvailable());
  externalLargest=1000000; assert(M4NativeProviderHeavyGate::tlsBlockAvailable());
  auto p=static_cast<unsigned char*>(allocHook(2,16384)); assert(p);
  for(int i=0;i<32768;i++) assert(p[i]==0);
  freeHook(p); assert(frees==1);
  // PSRAM OOM must not invoke a second internal-RAM allocation.
  fail=true; int before=calls; assert(!allocHook(1,16384)); assert(calls==before+1);
  before=calls; assert(!allocHook(SIZE_MAX,2)); assert(calls==before);
  // A block allocated before installation is still freed by the paired hook.
  freeHook(malloc(8)); assert(frees==2);
}
''')
    subprocess.run(["c++", "-std=c++17", "-DARDUINO_ARCH_ESP32=1", "-DCROSSPOINT_MURPHY_M4=1",
                    "-fsanitize=address,undefined", "-I", str(d),
                    "-I", str(ROOT / "firmware/src"), str(d / "test.cpp"),
                    "-o", str(d / "test")], check=True)
    subprocess.run([str(d / "test")], check=True)
print("TLS allocator capabilities, overflow, OOM and paired free: PASS")
