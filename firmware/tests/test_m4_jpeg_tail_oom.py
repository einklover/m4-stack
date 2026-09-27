#!/usr/bin/env python3
"""Host check: JPEG tail allocations fail closed and a normal convert still matches.

Builds the real JpegToBmpConverter and Atkinson ditherer with -fno-exceptions.
Each nothrow allocation in one scaled convert is refused in turn. The convert
must return false, and AddressSanitizer must not report a leak or abort.
"""

import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FW = ROOT / "firmware"

DRIVER = r"""
#include "SdFat.h"
#include "Print.h"
#include "JpegToBmpConverter.h"

#include <dlfcn.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

static int g_fail_nth = 0;
static int g_nothrow_calls = 0;

static void* tracked_malloc(std::size_t n) {
  using MallocFn = void* (*)(std::size_t);
  static MallocFn real = reinterpret_cast<MallocFn>(dlsym(RTLD_NEXT, "malloc"));
  return real(n);
}

static void* fail_or_alloc(std::size_t n) {
  ++g_nothrow_calls;
  if (g_fail_nth > 0 && g_nothrow_calls == g_fail_nth) return nullptr;
  if (n == 0) n = 1;
  return tracked_malloc(n);
}

void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return fail_or_alloc(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return fail_or_alloc(n); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

struct Captured {
  std::vector<uint8_t> bytes;
};

Captured* g_capture = nullptr;

size_t Print::write(uint8_t b) {
  if (!g_capture) return 0;
  g_capture->bytes.push_back(b);
  return 1;
}
size_t Print::write(const uint8_t* buf, size_t n) {
  if (!g_capture) return 0;
  g_capture->bytes.insert(g_capture->bytes.end(), buf, buf + n);
  return n;
}

static bool load_file(const char* path, std::vector<uint8_t>& out) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (n <= 0) {
    std::fclose(f);
    return false;
  }
  out.resize(static_cast<size_t>(n));
  const bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  const int mode = std::atoi(argv[1]);
  std::vector<uint8_t> jpeg;
  if (!load_file(argv[2], jpeg)) {
    std::fprintf(stderr, "fixture unreadable\n");
    return 2;
  }
  FsFile file;
  file.data = jpeg.data();
  file.size = jpeg.size();
  Print out;
  Captured captured;
  g_capture = &captured;

  if (mode == 0) {
    g_fail_nth = 0;
    g_nothrow_calls = 0;
    file.pos = 0;
    captured.bytes.clear();
    if (!JpegToBmpConverter::jpegFileToBmpStreamWithSize(file, out, 8, 6)) return 3;
    const int calls = g_nothrow_calls;
    std::vector<uint8_t> first = captured.bytes;
    g_nothrow_calls = 0;
    file.pos = 0;
    captured.bytes.clear();
    if (!JpegToBmpConverter::jpegFileToBmpStreamWithSize(file, out, 8, 6)) return 3;
    if (first != captured.bytes || first.size() < 70 || first[0] != 'B' || first[1] != 'M') return 4;
    g_nothrow_calls = 0;
    file.pos = 0;
    captured.bytes.clear();
    if (!JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(file, out, 8, 6)) return 5;
    if (captured.bytes.size() < 62 || captured.bytes[0] != 'B' || captured.bytes[1] != 'M') return 6;
    std::printf("normal calls=%d bmp2=%zu bmp1=%zu\n", calls, first.size(), captured.bytes.size());
    return 0;
  }

  g_fail_nth = mode;
  g_nothrow_calls = 0;
  file.pos = 0;
  captured.bytes.clear();
  const bool ok = JpegToBmpConverter::jpegFileToBmpStreamWithSize(file, out, 8, 6);
  std::printf("fail_nth=%d calls=%d ok=%d bytes=%zu\n", mode, g_nothrow_calls, ok ? 1 : 0, captured.bytes.size());
  if (ok) return 7;
  return 0;
}
"""

STUBS = {
    "HardwareSerial.h": """#pragma once
#include <cstdarg>
#include <cstdio>
struct HardwareSerial {
  void printf(const char*, ...) {}
};
static HardwareSerial Serial;
inline unsigned long millis() { return 0; }
""",
    "SdFat.h": """#pragma once
#include "Print.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
class FsFile {
 public:
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t pos = 0;
  explicit operator bool() const { return data != nullptr; }
  int read(void* dst, size_t n) {
    if (pos >= size) return 0;
    if (n > size - pos) n = size - pos;
    memcpy(dst, data + pos, n);
    pos += n;
    return static_cast<int>(n);
  }
};
""",
    "Print.h": """#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
struct Captured;
extern Captured* g_capture;
struct Captured;
class Print {
 public:
  size_t write(uint8_t b);
  size_t write(const uint8_t* buf, size_t n);
};
""",
    "M4MemoryManager.h": """#pragma once
#include <cstdlib>
namespace M4Memory {
inline void* allocScratch(size_t n) { return std::malloc(n ? n : 1); }
inline void free(void* p) { std::free(p); }
}
""",
}


def run(cmd, cwd):
    result = subprocess.run(cmd, cwd=cwd, text=True, capture_output=True)
    if result.returncode != 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise SystemExit(f"command failed ({result.returncode}): {' '.join(cmd)}")
    return result


def main():
    with tempfile.TemporaryDirectory(prefix="m4-jpeg-oom-") as temp_name:
        temp = Path(temp_name)
        for name, text in STUBS.items():
            (temp / name).write_text(text, encoding="utf-8")
        ppm = temp / "fixture.ppm"
        width, height = 32, 24
        pixels = bytearray()
        for y in range(height):
            for x in range(width):
                value = (x * 8 + y * 3) & 255
                pixels.extend((value, 255 - value, (x * y) & 255))
        ppm.write_bytes(f"P6\n{width} {height}\n255\n".encode() + bytes(pixels))
        jpg = temp / "fixture.jpg"
        run(
            [
                "convert",
                str(ppm),
                "-sampling-factor",
                "4:2:0",
                "-interlace",
                "none",
                "-quality",
                "90",
                str(jpg),
            ],
            temp,
        )
        driver = temp / "driver.cpp"
        driver.write_text(DRIVER, encoding="utf-8")
        binary = temp / "jpeg_oom"
        pico_obj = temp / "picojpeg.o"
        sanitizer = ["-fsanitize=address", "-fno-omit-frame-pointer"]
        run(
            ["gcc", "-std=c11", *sanitizer, "-Wno-unused-variable", "-Wno-shift-negative-value", f"-I{FW / 'lib/picojpeg'}", "-c", str(FW / "lib/picojpeg/picojpeg.c"), "-o", str(pico_obj)],
            temp,
        )
        run(
            [
                "g++",
                "-std=c++17",
                "-fno-exceptions",
                *sanitizer,
                "-Wall",
                "-Wextra",
                "-Wno-unused-const-variable",
                "-Wno-sign-compare",
                f"-I{temp}",
                f"-I{FW / 'lib/JpegToBmpConverter'}",
                f"-I{FW / 'lib/GfxRenderer'}",
                f"-I{FW / 'lib/picojpeg'}",
                str(driver),
                str(FW / "lib/JpegToBmpConverter/JpegToBmpConverter.cpp"),
                str(FW / "lib/GfxRenderer/BitmapHelpers.cpp"),
                str(pico_obj),
                "-o",
                str(binary),
            ],
            temp,
        )
        normal = run([str(binary), "0", str(jpg)], temp)
        print(normal.stdout.strip())
        calls = int(normal.stdout.split("calls=")[1].split()[0])
        if calls < 6:
            raise SystemExit(f"expected object + 3 dither rows + 2 accumulators, saw {calls}")
        for nth in range(1, calls + 1):
            failed = run([str(binary), str(nth), str(jpg)], temp)
            print(failed.stdout.strip())
    print("jpeg tail oom: PASS")


if __name__ == "__main__":
    main()
