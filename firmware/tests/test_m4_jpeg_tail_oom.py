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

  auto convert = [&](int depth) {
    file.pos = 0;
    captured.bytes.clear();
    g_nothrow_calls = 0;
    if (depth == 1) return JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(file, out, 8, 6);
    return JpegToBmpConverter::jpegFileToBmpStreamWithSize(file, out, 8, 6);
  };
  auto bmp_ok = [&](int depth) {
    const size_t min_size = depth == 1 ? 62u : 70u;
    return captured.bytes.size() >= min_size && captured.bytes[0] == 'B' && captured.bytes[1] == 'M';
  };

  if (mode == 0) {
    g_fail_nth = 0;
    if (!convert(2)) return 3;
    const int calls2 = g_nothrow_calls;
    std::vector<uint8_t> first = captured.bytes;
    if (!convert(2)) return 3;
    if (first != captured.bytes || !bmp_ok(2)) return 4;
    if (!convert(1)) return 5;
    const int calls1 = g_nothrow_calls;
    if (!bmp_ok(1)) return 6;
    std::printf("normal calls2=%d calls1=%d bmp2=%zu bmp1=%zu\n", calls2, calls1, first.size(),
                captured.bytes.size());
    return 0;
  }

  if (mode == -1) {
    if (argc < 5) return 2;
    g_fail_nth = 0;
    if (!convert(2) || !bmp_ok(2)) return 3;
    FILE* f2 = std::fopen(argv[3], "wb");
    if (!f2) return 2;
    std::fwrite(captured.bytes.data(), 1, captured.bytes.size(), f2);
    std::fclose(f2);
    if (!convert(1) || !bmp_ok(1)) return 5;
    FILE* f1 = std::fopen(argv[4], "wb");
    if (!f1) return 2;
    std::fwrite(captured.bytes.data(), 1, captured.bytes.size(), f1);
    std::fclose(f1);
    std::printf("dumped bmp2 bmp1\n");
    return 0;
  }

  const int depth = (argc >= 4) ? std::atoi(argv[3]) : 2;
  g_fail_nth = mode;
  const bool ok = convert(depth);
  std::printf("fail_nth=%d depth=%d calls=%d ok=%d bytes=%zu\n", mode, depth, g_nothrow_calls, ok ? 1 : 0,
              captured.bytes.size());
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


PARENT = "8cae8a98a80a8085c3b86c1fe43c250981ad06d5"


def compile_converter(temp, binary, pico_obj, converter_cpp, gfx_include):
    sanitizer = ["-fsanitize=address", "-fno-omit-frame-pointer"]
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
            f"-I{gfx_include}",
            f"-I{FW / 'lib/JpegToBmpConverter'}",
            f"-I{FW / 'lib/picojpeg'}",
            str(temp / "driver.cpp"),
            str(converter_cpp),
            str(FW / "lib/GfxRenderer/BitmapHelpers.cpp"),
            str(pico_obj),
            "-o",
            str(binary),
        ],
        temp,
    )


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
        compile_converter(temp, binary, pico_obj, FW / "lib/JpegToBmpConverter/JpegToBmpConverter.cpp", FW / "lib/GfxRenderer")
        normal = run([str(binary), "0", str(jpg)], temp)
        print(normal.stdout.strip())
        calls2 = int(normal.stdout.split("calls2=")[1].split()[0])
        calls1 = int(normal.stdout.split("calls1=")[1].split()[0])
        if calls2 < 6 or calls1 < 6:
            raise SystemExit(f"expected 6 nothrow sites for each depth, saw 2-bit {calls2} 1-bit {calls1}")
        for depth, calls in ((2, calls2), (1, calls1)):
            for nth in range(1, calls + 1):
                failed = run([str(binary), str(nth), str(jpg), str(depth)], temp)
                print(failed.stdout.strip())
        parent_gfx = temp / "parent_gfx"
        parent_gfx.mkdir()
        parent_cpp = temp / "parent_JpegToBmpConverter.cpp"
        parent_cpp.write_bytes(subprocess.check_output(["git", "show", f"{PARENT}:firmware/lib/JpegToBmpConverter/JpegToBmpConverter.cpp"], cwd=ROOT))
        (parent_gfx / "BitmapHelpers.h").write_bytes(subprocess.check_output(["git", "show", f"{PARENT}:firmware/lib/GfxRenderer/BitmapHelpers.h"], cwd=ROOT))
        parent_bin = temp / "jpeg_parent"
        compile_converter(temp, parent_bin, pico_obj, parent_cpp, parent_gfx)
        fixed_2, fixed_1 = temp / "fixed-2.bmp", temp / "fixed-1.bmp"
        parent_2, parent_1 = temp / "parent-2.bmp", temp / "parent-1.bmp"
        run([str(binary), "-1", str(jpg), str(fixed_2), str(fixed_1)], temp)
        run([str(parent_bin), "-1", str(jpg), str(parent_2), str(parent_1)], temp)
        if fixed_2.read_bytes() != parent_2.read_bytes() or fixed_1.read_bytes() != parent_1.read_bytes():
            raise SystemExit("fixed JPEG output differs from parent 8cae8a98 for 2-bit or 1-bit")
        print(f"parent match bmp2={fixed_2.stat().st_size} bmp1={fixed_1.stat().st_size}")
    print("jpeg tail oom: PASS")


if __name__ == "__main__":
    main()
