"""Compile the real BatchInstall inbox walk and paint path on the host.

The production worker body stays in BatchInstallActivity.cpp. This harness
extracts those methods and drives them with an SdFat-shaped directory cursor:
one parent, child closed before remove, delete does not rewind, getName
returns 0 when the UTF-8 name does not fit. No SD, PIO, QEMU, or device.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'firmware/src'
CPP = SRC / 'activities/apps/BatchInstallActivity.cpp'
H = SRC / 'activities/apps/BatchInstallActivity.h'


def function(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth = 1
    pos = brace + 1
    while depth:
        depth += (text[pos] == '{') - (text[pos] == '}')
        pos += 1
    return text[start:pos]


def adapt(body):
    # Only the extracted walk. std::snprintf / std::atomic / std::min stay real.
    return body.replace('std::string', 'TrackedString')


def run(code):
    with tempfile.TemporaryDirectory(prefix='m4-batch-install-') as tmp:
        cpp = Path(tmp) / 'test.cpp'
        exe = Path(tmp) / 'test'
        cpp.write_text(code)
        compiled = subprocess.run(
            ['c++', '-std=c++17', '-O1', '-g', '-pthread',
             '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
             '-I', str(SRC), str(cpp), '-o', str(exe)],
            capture_output=True, text=True)
        if compiled.returncode != 0:
            sys.stderr.write(compiled.stderr)
            sys.stderr.write(compiled.stdout)
            raise SystemExit(f'compile failed ({compiled.returncode})')
        ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        sys.stdout.write(ran.stdout)
        if ran.returncode != 0:
            sys.stderr.write(ran.stderr)
            raise SystemExit(f'actual batch install scan failed ({ran.returncode})')
    print('actual batch install scan: PASS')


def source_checks(cpp, header):
    assert 'scanInbox' not in cpp
    assert 'const auto paths' not in cpp
    for banned in ('displayTask', 'updateRequired_', 'renderingMutex_', 'M4xBatchUI',
                   'xSemaphore', 'vSemaphoreDelete'):
        assert banned not in cpp, banned
        assert banned not in header, banned
    assert '255 * 3 + 1' in cpp
    assert 'getName(name, sizeof(name))' in cpp
    assert 'nameLen == 0' in cpp
    assert '已处理 %d，已发现 %d' in cpp
    assert '正在处理 %d / %d' not in cpp
    assert 'xTaskCreate(batchTaskTrampoline, "M4xBatch", 12288' in cpp

    run_body = function(cpp, 'void runInboxBatch(BatchInstallActivity::Job* job)')
    assert run_body.count('SdMan.open(') == 1
    assert 'vector' not in run_body
    assert 'push_back' not in run_body
    assert 'seekSet' not in run_body
    assert 'rewind' not in run_body
    assert run_body.index('child.close()') < run_body.index('processInboxPackage')
    assert 'root.getError()' in run_body
    assert 'char name[kInboxNameBytes]' in run_body

    tramp = function(cpp, 'void batchTaskTrampoline(void* param)')
    for banned in ('vector', 'FsFile', 'string', 'scanInbox'):
        assert banned not in tramp, tramp
    assert tramp.index('runInboxBatch(') < tramp.index('done.store(') < tramp.index('vTaskDelete(')
    after = tramp.split('done.store(', 1)[1]
    for token in ('job', 'path', 'root', 'child', 'this'):
        assert token not in after, after

    on_exit = function(cpp, 'void BatchInstallActivity::onExit()')
    assert 'while (!job_->done.load' in on_exit
    assert on_exit.index('ActivityWithSubactivity::onExit()') < on_exit.index('while (!job_->done.load')
    assert 'vTaskDelete' not in on_exit
    assert 'displayTask' not in on_exit
    assert 'timeout' not in on_exit

    loop = function(cpp, 'void BatchInstallActivity::loop()')
    assert 'kIntervalMs = 500' in loop
    assert 'now.done' in loop
    assert 'render();' in loop

    assert cpp.count('vTaskDelete(') == 1
    assert 'freertos/task.h' not in header
    assert 'ProgressView' in header


HARNESS = r'''
#include "apps/M4xInboxBatch.h"
#include "apps/M4xPaths.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

void batchTaskTrampoline(void* param);

static int gFsLive = 0;
static int gFsDtors = 0;
static int gStringLive = 0;
static int gStringDtors = 0;
static int gAppLive = 0;
static int gAppDtors = 0;
static int gOpenChildren = 0;
static int gRootOpens = 0;
static int gSlotReads = 0;
static int gDelays = 0;
static int gRewinds = 0;
static int gReadFails = 0;
static int gEofHits = 0;
static int gSelfDeletes = 0;
static int gCursor = 0;
static int gReadFailAt = -1;
static bool gOpenFails = false;
static bool gNotDir = false;
static bool gIncludeOld = false;
static std::atomic<bool>* gDoneFlag = nullptr;
static unsigned long gMillis = 0;

static void require(bool ok, const char* msg) {
  if (!ok) {
    std::fprintf(stderr, "REQUIRE %s\n", msg);
    std::abort();
  }
}

static void requireInt(long long got, long long want, const char* msg) {
  if (got != want) {
    std::fprintf(stderr, "REQUIRE %s got=%lld want=%lld\n", msg, got, want);
    std::abort();
  }
}

struct TrackedString {
  std::string s;
  TrackedString() { gStringLive++; }
  TrackedString(const char* p) : s(p ? p : "") { gStringLive++; }
  TrackedString(const char* p, size_t n) : s(p ? p : "", p ? n : 0) { gStringLive++; }
  TrackedString(const TrackedString& o) : s(o.s) { gStringLive++; }
  TrackedString(TrackedString&& o) noexcept : s(std::move(o.s)) { gStringLive++; }
  TrackedString& operator=(const TrackedString& o) {
    if (this != &o) s = o.s;
    return *this;
  }
  TrackedString& operator=(TrackedString&& o) noexcept {
    if (this != &o) s = std::move(o.s);
    return *this;
  }
  ~TrackedString() {
    gStringLive--;
    gStringDtors++;
    if (gStringLive < 0) {
      std::fprintf(stderr, "string live negative\n");
      std::abort();
    }
    if (gDoneFlag && gDoneFlag->load()) {
      std::fprintf(stderr, "string dtor after done\n");
      std::abort();
    }
  }
  bool empty() const { return s.empty(); }
  char operator[](size_t i) const { return s[i]; }
  const char* c_str() const { return s.c_str(); }
  TrackedString operator+(const char* p) const {
    TrackedString out;
    out.s = s;
    if (p) out.s += p;
    return out;
  }
  TrackedString operator+(const TrackedString& o) const {
    TrackedString out;
    out.s = s + o.s;
    return out;
  }
};

struct Entry {
  std::string name;
  bool dir = false;
  bool nameFail = false;
  bool deleted = false;
};

static std::vector<Entry> gEntries;
static std::vector<std::string> gVisited;
static std::vector<int> gCursorLog;

struct FsFile {
  bool open = false;
  bool isDir = false;
  bool root = false;
  int slot = -1;
  uint8_t error = 0;

  FsFile() { gFsLive++; }
  FsFile(const FsFile&) = delete;
  FsFile& operator=(const FsFile&) = delete;
  FsFile(FsFile&& o) noexcept
      : open(o.open), isDir(o.isDir), root(o.root), slot(o.slot), error(o.error) {
    o.open = false;
    o.slot = -1;
    o.error = 0;
    gFsLive++;
  }
  FsFile& operator=(FsFile&& o) noexcept {
    if (this != &o) {
      if (open) close();
      open = o.open;
      isDir = o.isDir;
      root = o.root;
      slot = o.slot;
      error = o.error;
      o.open = false;
      o.slot = -1;
      o.error = 0;
    }
    return *this;
  }
  ~FsFile() {
    if (open) close();
    gFsLive--;
    gFsDtors++;
    if (gFsLive < 0) {
      std::fprintf(stderr, "FsFile live negative\n");
      std::abort();
    }
    if (gDoneFlag && gDoneFlag->load()) {
      std::fprintf(stderr, "FsFile dtor after done\n");
      std::abort();
    }
  }
  explicit operator bool() const { return open; }
  bool isDirectory() const { return isDir; }
  uint8_t getError() const { return open ? error : 0xFF; }
  void close() {
    if (!open) return;
    open = false;
    if (!root) {
      if (gOpenChildren <= 0) {
        std::fprintf(stderr, "child close underflow\n");
        std::abort();
      }
      gOpenChildren--;
    }
  }
  size_t getName(char* buf, size_t size) {
    if (slot < 0 || size == 0) {
      if (size) buf[0] = 0;
      return 0;
    }
    const Entry& e = gEntries[static_cast<size_t>(slot)];
    if (e.nameFail || size < e.name.size() + 1) {
      buf[0] = 0;
      return 0;
    }
    std::memcpy(buf, e.name.data(), e.name.size());
    buf[e.name.size()] = 0;
    return e.name.size();
  }
  FsFile openNextFile() {
    if (!root || !open) {
      std::fprintf(stderr, "openNext on a non-root\n");
      std::abort();
    }
    while (true) {
    if (gReadFailAt >= 0 && gCursor == gReadFailAt) {
      error = 1;
      gReadFails++;
      if (gReadFails > 2) {
        std::fprintf(stderr, "read-error spin\n");
        std::abort();
      }
      return FsFile();
    }
    if (gCursor >= static_cast<int>(gEntries.size())) {
      gEofHits++;
      if (gEofHits > 2) {
        std::fprintf(stderr, "eof spin\n");
        std::abort();
      }
      return FsFile();
    }
    const int slot = gCursor;
    if (gCursor + 1 < gCursor) gRewinds++;
    gCursor++;
    gSlotReads++;
    if (gSlotReads > static_cast<int>(gEntries.size()) + 2) {
      std::fprintf(stderr, "slotReads ran away opens=%d\n", gRootOpens);
      std::abort();
    }
    gCursorLog.push_back(gCursor);
    const Entry& e = gEntries[static_cast<size_t>(slot)];
    // SdFat openNext skips "." / ".." / deleted slots and keeps walking.
    // Returning a closed file here would look like EOF to the real walker.
    if (e.deleted || e.name == "." || e.name == "..") continue;
    if (gVisited.size() > gEntries.size() + 2) {
      std::fprintf(stderr, "walk repeated entries opens=%d visited=%zu\n", gRootOpens, gVisited.size());
      std::abort();
    }
    FsFile child;
    child.open = true;
    child.isDir = e.dir;
    child.slot = slot;
    gOpenChildren++;
    gVisited.push_back(e.name);
    return child;
    }
  }
};

struct SdManType {
  FsFile open(const char*) {
    gRootOpens++;
    if (gCursor > 0) gRewinds++;
    gCursor = 0;
    if (gOpenFails) return FsFile();
    FsFile root;
    root.open = true;
    root.root = true;
    root.isDir = !gNotDir;
    return root;
  }
  bool remove(const char* path) {
    if (gOpenChildren != 0) {
      std::fprintf(stderr, "remove while %d children open\n", gOpenChildren);
      std::abort();
    }
    const int cursor = gCursor;
    std::string full(path ? path : "");
    auto slash = full.find_last_of('/');
    std::string base = slash == std::string::npos ? full : full.substr(slash + 1);
    if (base == "bad-remove.m4x") {
      require(gCursor == cursor, "remove moved the parent cursor");
      return false;
    }
    for (auto& e : gEntries) {
      if (!e.deleted && e.name == base) {
        e.deleted = true;
        require(gCursor == cursor, "remove moved the parent cursor");
        return true;
      }
    }
    require(gCursor == cursor, "remove moved the parent cursor");
    return false;
  }
} SdMan;

struct SerialPort {
  void printf(const char*, ...) {}
} Serial;

static std::string baseOf(const TrackedString& path) {
  auto slash = path.s.find_last_of('/');
  if (slash == std::string::npos) return path.s;
  return path.s.substr(slash + 1);
}

static std::string idOf(const std::string& base) {
  if (base.size() >= 4 && base[base.size() - 4] == '.') return base.substr(0, base.size() - 4);
  return base;
}

struct M4xInstalledApp {
  TrackedString id;
  int versionCode = 0;
  M4xInstalledApp() { gAppLive++; }
  M4xInstalledApp(const M4xInstalledApp& o) : id(o.id), versionCode(o.versionCode) { gAppLive++; }
  M4xInstalledApp(M4xInstalledApp&& o) noexcept : id(std::move(o.id)), versionCode(o.versionCode) { gAppLive++; }
  M4xInstalledApp& operator=(const M4xInstalledApp& o) {
    id = o.id;
    versionCode = o.versionCode;
    return *this;
  }
  M4xInstalledApp& operator=(M4xInstalledApp&& o) noexcept {
    id = std::move(o.id);
    versionCode = o.versionCode;
    return *this;
  }
  ~M4xInstalledApp() {
    gAppLive--;
    gAppDtors++;
    if (gAppLive < 0) {
      std::fprintf(stderr, "app live negative\n");
      std::abort();
    }
    if (gDoneFlag && gDoneFlag->load()) {
      std::fprintf(stderr, "app dtor after done\n");
      std::abort();
    }
  }
};

struct M4xManifest {
  TrackedString id;
  int versionCode = 0;
};

struct M4xInstallResult {
  bool ok = false;
  TrackedString error;
  M4xManifest manifest;
};

class M4xRegistry {
 public:
  static std::vector<M4xInstalledApp> load() {
    std::vector<M4xInstalledApp> apps;
    M4xInstalledApp sentinel;
    sentinel.id = TrackedString("sentinel-never");
    apps.push_back(std::move(sentinel));
    if (gIncludeOld) {
      M4xInstalledApp old;
      old.id = TrackedString("old-ver");
      old.versionCode = 5;
      apps.push_back(std::move(old));
    }
    return apps;
  }
  static const M4xInstalledApp* find(const std::vector<M4xInstalledApp>& apps, const TrackedString& id) {
    for (const auto& app : apps) {
      if (app.id.s == id.s) return &app;
    }
    return nullptr;
  }
};

class M4xInstaller {
 public:
  static void ensureLayout() {}
  static M4xInstallResult probe(const TrackedString& path) {
    M4xInstallResult result;
    const std::string base = baseOf(path);
    if (base == "bad-probe.m4x") {
      result.ok = false;
      result.error = TrackedString("probe-failed");
      return result;
    }
    result.ok = true;
    result.manifest.id = TrackedString(idOf(base).c_str());
    result.manifest.versionCode = base == "old-ver.m4x" ? 1 : 2;
    return result;
  }
  static M4xInstallResult install(const TrackedString& path) {
    M4xInstallResult result;
    const std::string base = baseOf(path);
    if (base == "bad-install.m4x") {
      result.ok = false;
      result.error = TrackedString("install-failed");
      return result;
    }
    result.ok = true;
    return result;
  }
};

#define pdMS_TO_TICKS(ms) (ms)

void esp_task_wdt_reset() {}

void vTaskDelay(uint32_t ticks) {
  gDelays++;
  if (ticks >= 20) std::this_thread::sleep_for(std::chrono::milliseconds(ticks));
  else std::this_thread::yield();
}

void vTaskDelete(void*) {
  gSelfDeletes++;
  if (!gDoneFlag || !gDoneFlag->load() || gStringLive || gFsLive || gAppLive) {
    std::fprintf(stderr, "self-delete done=%d string=%d fs=%d app=%d\n",
                 gDoneFlag && gDoneFlag->load() ? 1 : 0, gStringLive, gFsLive, gAppLive);
    std::abort();
  }
}

struct Renderer {
  void clearScreen() const {}
  int getScreenWidth() const { return 480; }
  int getScreenHeight() const { return 800; }
  void fillRoundedRect(int, int, int, int, int, int) const {}
  void displayBuffer() const;
};

static std::vector<std::string> gFrame;
static std::vector<std::vector<std::string>> gFrames;

void Renderer::displayBuffer() const {
  gFrames.push_back(gFrame);
  gFrame.clear();
}

struct Rect {
  int x, y, w, h;
};

struct GuiType {
  void drawHeader(const Renderer&, Rect, const char*) const {}
} GUI;

namespace Color {
constexpr int Black = 0;
}
namespace EpdFontFamily {
constexpr int BOLD = 1;
}
constexpr int UI_12_FONT_ID = 12;
constexpr int UI_10_FONT_ID = 10;

struct Metrics {
  int topPadding;
  int headerHeight;
};

struct UITheme {
  static UITheme& getInstance() {
    static UITheme theme;
    return theme;
  }
  Metrics getMetrics() const { return Metrics{10, 40}; }
};

struct M4UiText {
  static void drawCentered(const Renderer&, int, int, const char* text, bool = true, int = 0) {
    gFrame.push_back(text ? text : "");
  }
  static void drawCenteredInBox(const Renderer&, int, int, int, int, int, const char*, bool, int, int) {}
};

class MappedInputManager {
 public:
  enum class Button { Back, Confirm };
  bool wasReleased(Button) const { return false; }
  bool wasBackGesture() const { return false; }
  bool hasTouch() const { return false; }
  bool wasScreenTapped(int&, int&) const { return false; }
};

class ActivityWithSubactivity {
 public:
  void onExit() {}
};

unsigned long millis() { return gMillis; }

class BatchInstallActivity : public ActivityWithSubactivity {
 public:
  struct Job {
    std::atomic<int> total{0};
    std::atomic<int> processed{0};
    std::atomic<int> installed{0};
    std::atomic<int> skipped{0};
    std::atomic<int> failed{0};
    std::atomic<bool> scanning{true};
    std::atomic<bool> done{false};
  };
  struct ProgressView {
    int total = 0;
    int processed = 0;
    int installed = 0;
    int skipped = 0;
    int failed = 0;
    bool scanning = false;
    bool done = false;
  };
  Job* job_ = nullptr;
  ProgressView painted_{};
  bool hasPaint_ = false;
  unsigned long lastPaintMs_ = 0;
  Renderer renderer;
  MappedInputManager mappedInput;
  std::function<void()> onDone_ = [] {};
  void onExit();
  void loop();
  void render() const;
};

struct ScanStats {
  int total = 0;
  int processed = 0;
  int installed = 0;
  int skipped = 0;
  int failed = 0;
  int opens = 0;
  int reads = 0;
  int delays = 0;
  int rewinds = 0;
  int kept = 0;
  int selfDelta = 0;
  int fsDtors = 0;
  int stringDtors = 0;
  int appDtors = 0;
  long ms = 0;
};

static int keptPackages() {
  int n = 0;
  for (const auto& e : gEntries) {
    if (e.deleted || e.name == "." || e.name == "..") continue;
    n++;
  }
  return n;
}

static void prepare(std::vector<Entry> entries) {
  gEntries = std::move(entries);
  gReadFailAt = -1;
  gOpenFails = false;
  gNotDir = false;
  gIncludeOld = false;
  gCursor = 0;
  gRootOpens = 0;
  gSlotReads = 0;
  gDelays = 0;
  gRewinds = 0;
  gReadFails = 0;
  gEofHits = 0;
  gVisited.clear();
  gCursorLog.clear();
}

static ScanStats runScan() {
  const int self0 = gSelfDeletes;
  const int fs0 = gFsDtors;
  const int st0 = gStringDtors;
  const int ap0 = gAppDtors;
  BatchInstallActivity::Job job;
  gDoneFlag = &job.done;
  const auto t0 = std::chrono::steady_clock::now();
  batchTaskTrampoline(&job);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
  gDoneFlag = nullptr;
  require(job.done.load(), "worker did not publish done");
  require(gStringLive == 0 && gFsLive == 0 && gAppLive == 0, "live objects after self-delete");
  require(gOpenChildren == 0, "child left open");
  ScanStats stats;
  stats.total = job.total.load();
  stats.processed = job.processed.load();
  stats.installed = job.installed.load();
  stats.skipped = job.skipped.load();
  stats.failed = job.failed.load();
  stats.opens = gRootOpens;
  stats.reads = gSlotReads;
  stats.delays = gDelays;
  stats.rewinds = gRewinds;
  stats.kept = keptPackages();
  stats.selfDelta = gSelfDeletes - self0;
  stats.fsDtors = gFsDtors - fs0;
  stats.stringDtors = gStringDtors - st0;
  stats.appDtors = gAppDtors - ap0;
  stats.ms = static_cast<long>(ms);
  requireInt(stats.selfDelta, 1, "self-delete count");
  require(stats.fsDtors > 0, "FsFile dtor missing");
  for (size_t i = 1; i < gCursorLog.size(); i++) {
    require(gCursorLog[i] > gCursorLog[i - 1], "parent cursor went backwards");
  }
  return stats;
}

static std::vector<Entry> mixedEntries(int n) {
  std::vector<Entry> entries;
  entries.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; i++) {
    Entry e;
    char buf[32];
    switch (i % 5) {
      case 0:
        std::snprintf(buf, sizeof(buf), "d%05d", i);
        e.dir = true;
        break;
      case 1:
        std::snprintf(buf, sizeof(buf), "f%05d.txt", i);
        break;
      case 2:
        std::snprintf(buf, sizeof(buf), "f%05d.zip", i);
        break;
      case 3:
        std::snprintf(buf, sizeof(buf), "f%05d.m4x", i);
        break;
      default:
        std::snprintf(buf, sizeof(buf), "f%05d.M4X", i);
        break;
    }
    e.name = buf;
    entries.push_back(std::move(e));
  }
  return entries;
}

static bool frameHas(size_t index, const char* needle) {
  require(index < gFrames.size(), "missing frame");
  for (const auto& line : gFrames[index]) {
    if (line.find(needle) != std::string::npos) return true;
  }
  return false;
}
'''

MAIN = r'''
int main() {
  prepare(mixedEntries(1000));
  ScanStats a = runScan();
  requireInt(a.total, 400, "1k m4x");
  requireInt(a.installed, 400, "1k installed");
  requireInt(a.failed, 0, "1k failed");
  requireInt(a.skipped, 0, "1k skipped");
  requireInt(a.processed, 400, "1k processed");
  requireInt(a.opens, 1, "1k opens");
  requireInt(a.reads, 1000, "1k reads");
  requireInt(a.delays, 1000, "1k delays");
  requireInt(a.rewinds, 0, "1k rewinds");
  requireInt(a.kept, 600, "1k left");
  require(a.stringDtors > 0 && a.appDtors > 0, "1k dtors");
  requireInt(static_cast<long long>(gVisited.size()), 1000, "1k visited");
  std::printf("mixed 1000: m4x=%d installed=%d failed=%d skipped=%d opens=%d reads=%d delays=%d left=%d\n",
              a.total, a.installed, a.failed, a.skipped, a.opens, a.reads, a.delays, a.kept);

  prepare(mixedEntries(10000));
  ScanStats b = runScan();
  requireInt(b.total, 4000, "10k m4x");
  requireInt(b.installed, 4000, "10k installed");
  requireInt(b.failed, 0, "10k failed");
  requireInt(b.skipped, 0, "10k skipped");
  requireInt(b.opens, 1, "10k opens");
  requireInt(b.reads, 10000, "10k reads");
  requireInt(b.delays, 10000, "10k delays");
  requireInt(b.rewinds, 0, "10k rewinds");
  requireInt(b.kept, 6000, "10k left");
  requireInt(static_cast<long long>(gVisited.size()), 10000, "10k visited");
  std::printf(
      "mixed 10000: m4x=%d installed=%d failed=%d skipped=%d opens=%d reads=%d delays=%d left=%d ms=%ld\n",
      b.total, b.installed, b.failed, b.skipped, b.opens, b.reads, b.delays, b.kept, b.ms);

  std::string longName;
  for (int i = 0; i < 200; i++) longName += "\xe4\xb8\x80";
  longName += ".m4x";
  require(longName.size() > 256 && longName.size() < 766, "long-name fixture size");
  prepare({Entry{longName, false, false, false}});
  ScanStats c = runScan();
  requireInt(c.installed, 1, "long-name installed");
  requireInt(c.failed, 0, "long-name failed");
  requireInt(c.kept, 0, "long-name left");
  requireInt(c.opens, 1, "long-name opens");
  std::printf("long-name: installed=%d left=%d bytes=%zu\n", c.installed, c.kept, longName.size());

  Entry tooLong;
  tooLong.name = std::string(766, 'B') + ".m4x";
  require(tooLong.name.size() > 765, "name-fail fixture size");
  Entry later;
  later.name = "later.m4x";
  prepare({tooLong, later});
  ScanStats d = runScan();
  requireInt(d.failed, 1, "name-fail failed");
  requireInt(d.installed, 1, "name-fail later installed");
  requireInt(d.total, 1, "name-fail discovered");
  requireInt(d.kept, 1, "name-fail kept");
  require(!gEntries[0].deleted, "truncated name was removed");
  require(gEntries[1].deleted, "later package was kept");
  requireInt(static_cast<long long>(gVisited.size()), 2, "name-fail did not continue");
  std::printf("name-fail: failed=%d kept=%d later_installed=%d\n", d.failed, d.kept, d.installed);

  prepare({Entry{"a.m4x", false, false, false}, Entry{"b.m4x", false, false, false},
           Entry{"c.m4x", false, false, false}});
  gReadFailAt = 2;
  ScanStats e = runScan();
  requireInt(e.installed, 2, "read-error installed");
  requireInt(e.failed, 1, "read-error failed");
  requireInt(e.total, 2, "read-error total");
  requireInt(e.processed, 2, "read-error processed");
  require(!gEntries[2].deleted, "unread tail was removed");
  int visitedC = 0;
  for (const auto& name : gVisited)
    if (name == "c.m4x") visitedC++;
  requireInt(visitedC, 0, "read-error visited c");
  std::printf("read-error: installed=%d failed=%d visited_c=%d\n", e.installed, e.failed, visitedC);

  prepare({});
  ScanStats f = runScan();
  requireInt(f.failed, 0, "empty failed");
  requireInt(f.total, 0, "empty total");
  requireInt(f.opens, 1, "empty opens");
  std::printf("eof-empty: failed=%d total=%d\n", f.failed, f.total);

  prepare({});
  gOpenFails = true;
  ScanStats g = runScan();
  requireInt(g.failed, 1, "root-open-fail failed");
  requireInt(g.total, 0, "root-open-fail total");
  requireInt(g.reads, 0, "root-open-fail reads");
  std::printf("root-open-fail: failed=%d total=%d\n", g.failed, g.total);

  prepare({Entry{"should-not-read.m4x", false, false, false}});
  gNotDir = true;
  ScanStats h = runScan();
  requireInt(h.failed, 1, "root-not-dir failed");
  requireInt(h.total, 0, "root-not-dir total");
  requireInt(h.reads, 0, "root-not-dir reads");
  require(!gEntries[0].deleted, "root-not-dir removed a package");
  std::printf("root-not-dir: failed=%d\n", h.failed);

  prepare({Entry{".", true, false, false}, Entry{"..", true, false, false},
           Entry{"ok-a.m4x", false, false, false}, Entry{"bad-probe.m4x", false, false, false},
           Entry{"note.txt", false, false, false}, Entry{"old-ver.m4x", false, false, false},
           Entry{"bad-install.m4x", false, false, false}, Entry{"bad-remove.m4x", false, false, false},
           Entry{"ok-b.m4x", false, false, false}});
  gIncludeOld = true;
  const int selfBefore = gSelfDeletes;
  ScanStats k = runScan();
  const std::vector<std::string> want = {"ok-a.m4x", "bad-probe.m4x", "note.txt", "old-ver.m4x",
                                         "bad-install.m4x", "bad-remove.m4x", "ok-b.m4x"};
  require(gVisited == want, "delete-cursor visit list");
  requireInt(k.opens, 1, "delete-cursor opens");
  requireInt(k.rewinds, 0, "delete-cursor rewinds");
  requireInt(k.reads, 9, "delete-cursor reads");
  requireInt(k.installed, 2, "delete-cursor installed");
  requireInt(k.skipped, 1, "delete-cursor skipped");
  requireInt(k.failed, 3, "delete-cursor failed");
  requireInt(k.total, 6, "delete-cursor total");
  requireInt(k.processed, 6, "delete-cursor processed");
  requireInt(k.kept, 4, "delete-cursor kept");
  require(gEntries[2].deleted && gEntries[5].deleted && gEntries[8].deleted, "success/old not removed");
  require(!gEntries[3].deleted && !gEntries[4].deleted && !gEntries[6].deleted && !gEntries[7].deleted,
          "failed package removed");
  require(k.fsDtors > 0 && k.stringDtors > 0 && k.appDtors > 0, "delete-cursor dtors");
  std::printf("delete-cursor: visited=%zu rewinds=%d opens=%d installed=%d skipped=%d failed=%d kept=%d\n",
              gVisited.size(), k.rewinds, k.opens, k.installed, k.skipped, k.failed, k.kept);
  std::printf("dtors: fs=%d strings=%d apps=%d self_delete=%d\n", k.fsDtors, k.stringDtors, k.appDtors,
              gSelfDeletes - selfBefore);

  BatchInstallActivity ui;
  BatchInstallActivity::Job view;
  ui.job_ = &view;
  gMillis = 0;
  ui.loop();
  requireInt(static_cast<long long>(gFrames.size()), 1, "initial render");
  require(frameHas(0, "正在扫描"), "initial frame");
  for (int i = 0; i < 15; i++) {
    gMillis = 10;
    ui.loop();
  }
  requireInt(static_cast<long long>(gFrames.size()), 1, "same progress render");
  view.total.store(4);
  view.processed.store(1);
  view.scanning.store(false);
  gMillis = 10;
  ui.loop();
  requireInt(static_cast<long long>(gFrames.size()), 1, "rate-limited render");
  gMillis = 500;
  ui.loop();
  requireInt(static_cast<long long>(gFrames.size()), 2, "later render");
  require(frameHas(1, "已发现"), "discovered line");
  require(!frameHas(1, "正在处理"), "old fraction line");
  ui.loop();
  requireInt(static_cast<long long>(gFrames.size()), 2, "repeat after paint");
  view.done.store(true);
  gMillis = 500;
  ui.loop();
  requireInt(static_cast<long long>(gFrames.size()), 3, "completion render");
  require(frameHas(2, "安装"), "completion line");
  ui.loop();
  requireInt(static_cast<long long>(gFrames.size()), 3, "completion repeat");
  std::printf("render: initial=1 same=1 limited=1 later=%zu done=%zu\n", gFrames.size() - 1, gFrames.size());

  const int selfAtExit = gSelfDeletes;
  BatchInstallActivity leaving;
  auto* exitJob = new BatchInstallActivity::Job();
  leaving.job_ = exitJob;
  std::thread worker([exitJob] {
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    exitJob->done.store(true, std::memory_order_release);
  });
  const auto exit0 = std::chrono::steady_clock::now();
  leaving.onExit();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - exit0)
                           .count();
  worker.join();
  require(leaving.job_ == nullptr, "onExit kept the job");
  require(elapsed >= 30, "onExit did not wait");
  requireInt(gSelfDeletes, selfAtExit, "onExit self-deleted");
  std::printf("exit-wait: elapsed_ms=%lld\n", static_cast<long long>(elapsed));
  return 0;
}
'''


def main():
    cpp = CPP.read_text()
    header = H.read_text()
    source_checks(cpp, header)
    parts = []
    for signature in (
        'bool isM4xName(const char* name, size_t length)',
        'void processInboxPackage(BatchInstallActivity::Job* job, const std::string& path)',
        'void runInboxBatch(BatchInstallActivity::Job* job)',
        'void batchTaskTrampoline(void* param)',
    ):
        parts.append(adapt(function(cpp, signature)))
    for signature in (
        'void BatchInstallActivity::onExit()',
        'void BatchInstallActivity::loop()',
        'void BatchInstallActivity::render() const',
    ):
        parts.append(function(cpp, signature))
    run(HARNESS + '\n' + '\n'.join(parts) + '\n' + MAIN)


if __name__ == '__main__':
    main()
