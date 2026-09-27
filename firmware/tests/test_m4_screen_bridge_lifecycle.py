"""Compile the real ScreenBridge / NativeApp lifecycle methods on the host.

The production run() body stays in the firmware file: it pulls in ArduinoJson,
SD and Wi-Fi. This harness extracts the methods that own the worker lifetime
and drives them with a requestSmall shim that would block for 2.5 s unless the
real cancel lambda sees stop_. No network, SD, PIO or device.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'firmware/src'

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
    # Track lock/unlock around the real statements. Firmware keeps std::mutex.
    return (body
            .replace('std::lock_guard<std::mutex>', 'std::lock_guard<TrackedMutex>')
            .replace('std::unique_lock<std::mutex>', 'std::unique_lock<TrackedMutex>'))

def run(code, name):
    with tempfile.TemporaryDirectory(prefix='m4-screen-bridge-') as tmp:
        cpp = Path(tmp) / 'test.cpp'
        exe = Path(tmp) / 'test'
        cpp.write_text(code)
        compiled = subprocess.run(
            ['c++', '-std=c++17', '-O1', '-g', '-pthread',
             '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
             str(cpp), '-o', str(exe)],
            capture_output=True, text=True)
        if compiled.returncode != 0:
            sys.stderr.write(compiled.stderr)
            sys.stderr.write(compiled.stdout)
            raise SystemExit(f'compile failed ({compiled.returncode})')
        ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        sys.stdout.write(ran.stdout)
        if ran.returncode != 0:
            sys.stderr.write(ran.stderr)
            raise SystemExit(f'{name} failed ({ran.returncode})')
    print(name + ': PASS')

def source_checks(sb, native_h, native_cpp, ui, main_cpp):
    for line in ui.splitlines():
        if 'requestStop' in line or 'readyForDestruction' in line:
            assert '= 0' not in line, line
    assert 'virtual void requestStop() {}' in ui
    assert 'virtual bool readyForDestruction() const { return true; }' in ui
    assert 'virtual void pollAsync() {}' in ui

    assert 'bool readyForDestruction() const override;' in native_h
    on_exit = function(native_cpp, 'void NativeAppActivity::onExit()')
    assert on_exit.index('ActivityWithSubactivity::onExit()') < on_exit.index('requestStop()')
    assert on_exit.index('requestStop()') < on_exit.index('document_ = {}')
    assert 'controller_.reset' not in on_exit
    assert 'reset()' not in on_exit
    ready = function(native_cpp, 'bool NativeAppActivity::readyForDestruction() const')
    assert ready.index('ActivityWithSubactivity::readyForDestruction()') < ready.index(
        'controller_->readyForDestruction()')

    assert 'stopWorker' not in sb
    assert 'deleteTask(handle)' not in sb
    assert 'deleteTask(task_' not in sb
    assert sb.count('M4Psram::deleteTask(') == 1
    assert sb.count('vTaskDelay(') == 1
    for delay in ('pdMS_TO_TICKS(1800)', 'pdMS_TO_TICKS(900)', 'pdMS_TO_TICKS(700)', 'pdMS_TO_TICKS(40)'):
        assert delay not in sb, delay
    for call in ('delayUnlessStopped(1800)', 'delayUnlessStopped(900)',
                 'delayUnlessStopped(700)', 'delayUnlessStopped(40)'):
        assert call in sb, call

    dtor = function(sb, '~ScreenBridgeController() override')
    for banned in ('deleteTask', 'vTaskDelay', 'while', 'lock', 'stop_'):
        assert banned not in dtor, dtor

    delay = function(sb, 'bool delayUnlessStopped(uint32_t totalMs)')
    pre = delay.index('if (stop_.load(std::memory_order_acquire)) return false;')
    sleep = delay.index('vTaskDelay(')
    post = delay.rindex('return !stop_.load(std::memory_order_acquire);')
    assert pre < sleep < post
    assert 'lock_guard' not in delay
    assert 'unique_lock' not in delay

    stop = function(sb, 'void requestStop() override')
    assert stop.count('stop_.store(true, std::memory_order_release);') == 1
    assert 'mu_' not in stop
    assert 'lock' not in stop

    ready_sb = function(sb, 'bool readyForDestruction() const override')
    assert 'std::try_to_lock' in ready_sb
    assert 'if (!lock.owns_lock()) return false;' in ready_sb
    assert 'return task_ == nullptr;' in ready_sb
    assert 'lock_guard' not in ready_sb
    assert ready_sb.index('owns_lock()') < ready_sb.index('task_ == nullptr')

    pub = function(sb, 'void publishWorkerDone()')
    clear = pub.index('task_ = nullptr')
    close = pub.index('}', clear)
    dele = pub.index('M4Psram::deleteTask(nullptr)')
    assert clear < close < dele
    after = pub.split('M4Psram::deleteTask(nullptr);', 1)[1]
    for token in ('task_', 'mu_', 'status_', 'stop_', 'this->'):
        assert token not in after, after

    loop = function(sb, 'void workerLoop()')
    assert loop.split('publishWorkerDone();', 1)[1].strip() == '}'
    assert 'vTaskDelay(' not in loop

    produced = function(sb, 'void run(Job job, const std::string& key)')
    assert 'vTaskDelay(' not in produced
    assert 'if (!delayUnlessStopped(1800)) return;' in produced
    assert 'if (!delayUnlessStopped(900)) return;' in produced
    assert 'if (!delayUnlessStopped(700)) return;' in produced
    assert produced.index('if (stop_.load(std::memory_order_acquire)) return;') < produced.index('++revision_')
    assert 'if (!delayUnlessStopped(900)) return false;' in sb
    assert 'if (!delayUnlessStopped(700)) return false;' in sb

    req = function(sb, 'bool request(const char* method, const std::string& path, std::string& body, size_t cap)')
    assert 'timeoutMs = 20000' in req
    assert 'return stop_.load(std::memory_order_acquire);' in req
    assert 'lock_guard' not in req
    assert 'mu_' not in req
    start = function(sb, 'void startWorker()')
    assert 'if (task_ || stop_.load(std::memory_order_acquire)) return;' in start
    assert 'std::lock_guard<std::mutex> lock(mu_);' in start
    assert '无法启动屏幕桥任务' in start
    assert 'M4Psram::deleteTask' not in start
    ensure = function(sb, 'bool ensureEndpoint()')
    assert 'return stop_.load(std::memory_order_acquire);' in ensure
    assert 'lock_guard' not in ensure
    assert 'std::atomic<bool> stop_{false};' in sb
    assert 'return stop_;' not in sb
    assert 'stop_ = true' not in sb
    produced_loop = function(sb, 'void workerLoop()')
    assert 'if (stop_.load(std::memory_order_acquire)) break;' in produced_loop
    assert 'std::lock_guard<std::mutex> lock(mu_);' in function(sb, 'void publishWorkerDone()')

    helper = function(main_cpp, 'static void releaseM4HomeBoundaryResources()')
    assert 'Activities join their own workers' not in helper
    gate = helper.index('if (providerBusy || hasDeferredActivities())')
    shut = helper.index('M4HttpTransport::shutdown()')
    assert gate < shut
    assert 'else' in helper[gate:shut]
    loop_at = main_cpp.index('if (gM4PendingTransientReset && !hasDeferredActivities())')
    shut_at = main_cpp.index('M4HttpTransport::shutdown();', loop_at)
    between = main_cpp[loop_at:shut_at]
    assert '!m4HomeBoundaryWorkersBusy()' in between
    assert 'M4Memory::resetTransient();' in main_cpp[shut_at:shut_at + 160]

def main():
    sb = (SRC / 'apps/native/M4ScreenBridgeController.cpp').read_text()
    native_h = (SRC / 'activities/apps/NativeAppActivity.h').read_text()
    native_cpp = (SRC / 'activities/apps/NativeAppActivity.cpp').read_text()
    ui = (SRC / 'apps/native/M4NativeUiController.h').read_text()
    main_cpp = (SRC / 'main.cpp').read_text()
    source_checks(sb, native_h, native_cpp, ui, main_cpp)
    helper = function(main_cpp, 'static void releaseM4HomeBoundaryResources()')
    has_deferred = function(main_cpp, 'static bool hasDeferredActivities()')
    pending_at = main_cpp.index('if (gM4PendingTransientReset && !hasDeferredActivities())')
    pending_shut = main_cpp.index('M4HttpTransport::shutdown();', pending_at)
    pending_decision = main_cpp[pending_at:pending_shut + len('M4HttpTransport::shutdown();')]

    hooks = '\n'.join([
        '  virtual void requestStop() {}',
        '  virtual bool readyForDestruction() const { return true; }',
        '  virtual void pollAsync() {}',
    ])
    pieces = '\n'.join(adapt(function(sb, sig)) for sig in [
        'void requestStop() override',
        'bool readyForDestruction() const override',
        'void pollAsync() override',
        'void startWorker()',
        'bool delayUnlessStopped(uint32_t totalMs)',
        'void publishWorkerDone()',
        'void workerLoop()',
        'bool request(const char* method, const std::string& path, std::string& body, size_t cap)',
    ])
    native_on_exit = function(native_cpp, 'void NativeAppActivity::onExit()')
    native_ready = function(native_cpp, 'bool NativeAppActivity::readyForDestruction() const')

    code = r'''
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#define CHECK(cond) do { if (!(cond)) { \
  std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #cond); \
  std::abort(); } } while (0)

#define ARDUINO_ARCH_ESP32
#define pdMS_TO_TICKS(ms) (ms)
using TaskHandle_t = void*;
constexpr int pdPASS = 1;
constexpr int pdFAIL = 0;
constexpr int kSimulatedNetworkMs = 2500;
constexpr const char* kUserAgent = "Murphy-M4 ScreenBridge/2";

static std::atomic<int> createCalls{0};
static std::atomic<int> selfDeletes{0};
static std::atomic<int> foreignDeletes{0};
static std::atomic<bool> failCreate{false};
static std::atomic<bool> fastSuccess{false};
static std::atomic<bool> gClearedUnderLock{false};
static std::atomic<bool> gSealed{false};
static std::atomic<int> gWorkerLockDepth{0};
static std::thread::id gWorkerId{};
static void (*recordedFn)(void*) = nullptr;
static void* recordedArg = nullptr;
static std::atomic<int> liveGates{0};
static std::atomic<int> liveBufs{0};
static std::atomic<int> gateCtors{0};
static std::atomic<int> gateDtors{0};
static std::atomic<int> bufCtors{0};
static std::atomic<int> bufDtors{0};
static std::atomic<int> slicesDone{0};
static std::atomic<int> cancelledAt{-1};
static std::atomic<bool> finishedNaturally{false};
static std::atomic<bool> entered{false};
static std::atomic<int> runEntered{0};
static std::atomic<bool> delayReturnedFalse{false};
static std::atomic<bool> holdingMu{false};
static std::atomic<bool> releaseHold{false};
static std::atomic<int> followOn{0};
static std::atomic<int> delayEntries{0};
static std::atomic<int> gShutdowns{0};
static std::atomic<bool> gProvidersBusy{false};
static std::atomic<bool> gSleepDelays{true};
static std::atomic<unsigned long> gMillis{0};
static std::function<void()> onDelay;
static bool gM4PendingTransientReset = false;

struct Activity;
static Activity* deferredActivities = nullptr;

struct TrackedMutex {
  std::mutex m;
  void lock() {
    if (gSealed.load() && std::this_thread::get_id() == gWorkerId) {
      std::fprintf(stderr, "worker locked mu_ after self-delete\n");
      std::abort();
    }
    m.lock();
    if (std::this_thread::get_id() == gWorkerId) gWorkerLockDepth.fetch_add(1);
  }
  bool try_lock() {
    if (gSealed.load() && std::this_thread::get_id() == gWorkerId) {
      std::fprintf(stderr, "worker locked mu_ after self-delete\n");
      std::abort();
    }
    if (!m.try_lock()) return false;
    if (std::this_thread::get_id() == gWorkerId) gWorkerLockDepth.fetch_add(1);
    return true;
  }
  void unlock() {
    if (std::this_thread::get_id() == gWorkerId) gWorkerLockDepth.fetch_sub(1);
    m.unlock();
  }
};

struct TaskSlot {
  TaskHandle_t h = nullptr;
  TaskSlot& assign(TaskHandle_t v) {
    if (v == nullptr) {
      CHECK(std::this_thread::get_id() == gWorkerId);
      CHECK(gWorkerLockDepth.load() > 0);
      gClearedUnderLock.store(true);
    }
    h = v;
    return *this;
  }
  TaskSlot& operator=(TaskHandle_t v) { return assign(v); }
  TaskSlot& operator=(std::nullptr_t) { return assign(nullptr); }
  bool operator==(std::nullptr_t) const { return h == nullptr; }
  bool operator!=(std::nullptr_t) const { return h != nullptr; }
  operator bool() const { return h != nullptr; }
};

inline void vTaskDelay(uint32_t ticks) {
  delayEntries.fetch_add(1);
  if (onDelay) onDelay();
  if (gSleepDelays.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ticks));
  }
  gMillis.fetch_add(ticks);
}

static unsigned long millis() { return gMillis.load(); }

static bool m4HomeBoundaryWorkersBusy() { return gProvidersBusy.load(); }

namespace M4HttpTransport {
inline void shutdown() { gShutdowns.fetch_add(1); }
}
namespace M4NativeProviderManager { inline void cancelForeground() {} }
namespace M4NativeProviderCatalog { inline void cancel() {} }
namespace M4NativeProviderDiscovery { inline void cancel() {} }
namespace M4NativeProviderBookDetailAsync { inline void cancel() {} }
namespace M4NativeProviderLogin { inline void cancel() {} }
struct SerialType { void println(const char*) {} };
static SerialType Serial;

struct Gate {
  Gate() { liveGates.fetch_add(1); gateCtors.fetch_add(1); }
  ~Gate() { liveGates.fetch_sub(1); gateDtors.fetch_add(1); }
};
struct LocalBuf {
  explicit LocalBuf(size_t n) : data(n, 'n') { liveBufs.fetch_add(1); bufCtors.fetch_add(1); }
  ~LocalBuf() { liveBufs.fetch_sub(1); bufDtors.fetch_add(1); }
  std::string data;
};

namespace M4Psram {
inline int createTask(void (*fn)(void*), const char*, uint32_t, void* arg, int, TaskHandle_t* out) {
  createCalls.fetch_add(1);
  if (failCreate.load()) {
    if (out) *out = nullptr;
    return pdFAIL;
  }
  if (out) *out = reinterpret_cast<TaskHandle_t>(static_cast<uintptr_t>(0x21));
  recordedFn = fn;
  recordedArg = arg;
  return pdPASS;
}
inline void deleteTask(TaskHandle_t handle) {
  if (handle != nullptr) {
    foreignDeletes.fetch_add(1);
    std::fprintf(stderr, "force-delete of a live worker\n");
    std::abort();
  }
  CHECK(gClearedUnderLock.load());
  CHECK(gWorkerLockDepth.load() == 0);
  CHECK(liveGates.load() == 0);
  CHECK(liveBufs.load() == 0);
  CHECK(gateCtors.load() == gateDtors.load());
  CHECK(bufCtors.load() == bufDtors.load());
  gSealed.store(true);
  selfDeletes.fetch_add(1);
}
}  // namespace M4Psram

namespace M4NativeProviderHttp {
struct Header { std::string name; std::string value; };
struct HeaderList {
  HeaderList& operator=(std::initializer_list<Header>) { return *this; }
};
struct Request {
  std::string method;
  std::string url;
  HeaderList headers;
  size_t maxBytes = 0;
  uint32_t timeoutMs = 0;
};
struct Result { int status = 0; };
using CancelFn = std::function<bool()>;
inline bool requestSmall(const Request&, std::string& bodyOut, Result& resultOut, size_t,
                         const CancelFn& cancelled) {
  Gate gate;
  LocalBuf buf(256);
  entered.store(true);
  if (fastSuccess.load()) {
    bodyOut = "{\"ok\":true}";
    resultOut.status = 200;
    return true;
  }
  for (int i = 0; i < kSimulatedNetworkMs / 20; ++i) {
    slicesDone.fetch_add(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (cancelled && cancelled()) {
      cancelledAt.store(i);
      return false;
    }
  }
  finishedNaturally.store(true);
  bodyOut = "{\"ok\":true}";
  resultOut.status = 200;
  return true;
}
}  // namespace M4NativeProviderHttp

class Controller {
 public:
  virtual ~Controller() = default;
''' + hooks + r'''
};

class Bridge : public Controller {
 public:
  enum class Job : uint8_t { None = 0, LoadApps };
  mutable TrackedMutex mu_;
  std::atomic<bool> stop_{false};
  TaskSlot task_{};
  std::string status_ = "正在连接手机…";
  std::string selectedKey_;
  std::string base_;
  uint32_t revision_ = 1;
  Job pending_ = Job::None;
  bool initialQueued_ = false;
  int mode = 0;

  static void taskMain(void* arg) { static_cast<Bridge*>(arg)->workerLoop(); }
  bool ensureEndpoint() { std::fprintf(stderr, "ensureEndpoint\n"); std::abort(); return false; }
  void run(Job, const std::string&) {
    runEntered.fetch_add(1);
    if (mode == 2) {
      delayReturnedFalse.store(!delayUnlessStopped(5000));
      return;
    }
    if (mode == 3) {
      std::lock_guard<TrackedMutex> lock(mu_);
      holdingMu.store(true);
      auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
      while (!releaseHold.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      return;
    }
    if (mode == 4) {
      if (delayUnlessStopped(20)) followOn.fetch_add(1);
      return;
    }
    std::string body;
    request("GET", "/v2/apps", body, 4096);
  }

''' + pieces + r'''
};

struct ActivityWithSubactivity {
  int parentExits = 0;
  bool parentReady = true;
  void onExit() { ++parentExits; }
  bool readyForDestruction() const { return parentReady; }
};

struct StopController : Controller {
  int stops = 0;
  bool ready = false;
  void requestStop() override { ++stops; }
  bool readyForDestruction() const override { return ready; }
};

class NativeAppActivity : public ActivityWithSubactivity {
 public:
  std::unique_ptr<Controller> controller_;
  std::string document_ = "visible";
  void onExit();
  bool readyForDestruction() const;
};

''' + native_on_exit + '\n' + native_ready + r'''

static void resetRuntime() {
  createCalls.store(0);
  selfDeletes.store(0);
  foreignDeletes.store(0);
  failCreate.store(false);
  fastSuccess.store(false);
  gClearedUnderLock.store(false);
  gSealed.store(false);
  gWorkerLockDepth.store(0);
  gWorkerId = std::thread::id{};
  recordedFn = nullptr;
  recordedArg = nullptr;
  liveGates.store(0);
  liveBufs.store(0);
  gateCtors.store(0);
  gateDtors.store(0);
  bufCtors.store(0);
  bufDtors.store(0);
  slicesDone.store(0);
  cancelledAt.store(-1);
  finishedNaturally.store(false);
  entered.store(false);
  runEntered.store(0);
  delayReturnedFalse.store(false);
  holdingMu.store(false);
  releaseHold.store(false);
  followOn.store(0);
  delayEntries.store(0);
  gShutdowns.store(0);
  gProvidersBusy.store(false);
  gSleepDelays.store(true);
  gMillis.store(0);
  onDelay = nullptr;
  gM4PendingTransientReset = false;
  deferredActivities = nullptr;
}

static std::thread spawn() {
  auto fn = recordedFn;
  auto arg = recordedArg;
  CHECK(fn != nullptr);
  return std::thread([fn, arg] {
    gWorkerId = std::this_thread::get_id();
    fn(arg);
  });
}

static int64_t msSince(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t).count();
}
static int64_t usSince(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - t).count();
}

static void waitFlag(const std::atomic<int>& flag, int want, int64_t budgetMs) {
  auto t = std::chrono::steady_clock::now();
  while (flag.load() < want && msSince(t) < budgetMs) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

''' + has_deferred + '\n' + helper + r'''

static void existingPendingResetPath() {
''' + pending_decision + r'''
    }
  }
}

int main() {
  CHECK(kSimulatedNetworkMs > 2000);

  struct OtherController : Controller {};
  OtherController untouched;
  untouched.requestStop();
  CHECK(untouched.readyForDestruction());

  {
    NativeAppActivity bare;
    bare.onExit();
    CHECK(bare.parentExits == 1);
    CHECK(bare.controller_ == nullptr);
    CHECK(bare.document_.empty());
    CHECK(bare.readyForDestruction());
  }
  {
    NativeAppActivity app;
    auto* held = new StopController;
    app.controller_.reset(held);
    app.parentReady = true;
    held->ready = false;
    CHECK(!app.readyForDestruction());
    app.onExit();
    CHECK(app.parentExits == 1);
    CHECK(held->stops == 1);
    CHECK(app.controller_.get() == held);
    CHECK(app.document_.empty());
    CHECK(!app.readyForDestruction());
    app.onExit();
    CHECK(app.parentExits == 2);
    CHECK(held->stops == 2);
    CHECK(app.controller_.get() == held);
    held->ready = true;
    CHECK(app.readyForDestruction());
    app.parentReady = false;
    CHECK(!app.readyForDestruction());
    app.parentReady = true;
    app.controller_.reset();
    CHECK(app.readyForDestruction());
  }

  {
    auto t0 = std::chrono::steady_clock::now();
    Bridge idle;
    CHECK(idle.readyForDestruction());
    idle.requestStop();
    idle.requestStop();
    CHECK(idle.stop_);
    CHECK(idle.readyForDestruction());
    CHECK(selfDeletes.load() == 0);
    CHECK(createCalls.load() == 0);
    idle.pollAsync();
    CHECK(createCalls.load() == 0);
    CHECK(idle.readyForDestruction());
    CHECK(msSince(t0) < 200);
  }

  {
    resetRuntime();
    failCreate.store(true);
    Bridge failed;
    failed.pollAsync();
    CHECK(failed.status_ == "无法启动屏幕桥任务");
    CHECK(failed.revision_ == 2);
    CHECK(failed.task_ == nullptr);
    CHECK(failed.readyForDestruction());
    CHECK(createCalls.load() == 1);
    CHECK(recordedFn == nullptr);
    CHECK(selfDeletes.load() == 0);
    CHECK(foreignDeletes.load() == 0);
  }

  {
    resetRuntime();
    auto* bridge = new Bridge;
    bridge->mode = 2;
    bridge->pollAsync();
    CHECK(!bridge->readyForDestruction());
    CHECK(createCalls.load() == 1);
    auto th = spawn();
    auto started = std::chrono::steady_clock::now();
    waitFlag(runEntered, 1, 2000);
    CHECK(runEntered.load() >= 1);
    auto stopAt = std::chrono::steady_clock::now();
    bridge->requestStop();
    auto requestStopUs = usSince(stopAt);
    CHECK(requestStopUs < 50000);
    CHECK(selfDeletes.load() == 0);
    waitFlag(selfDeletes, 1, 2000);
    auto elapsed = msSince(started);
    CHECK(selfDeletes.load() == 1);
    CHECK(delayReturnedFalse.load());
    CHECK(bridge->readyForDestruction());
    CHECK(foreignDeletes.load() == 0);
    CHECK(elapsed < 1000);
    std::printf("delay cancel: request_stop_us=%lld elapsed_ms=%lld\n",
                static_cast<long long>(requestStopUs), static_cast<long long>(elapsed));
    delete bridge;
    th.join();
    CHECK(foreignDeletes.load() == 0);
  }

  {
    resetRuntime();
    auto* bridge = new Bridge;
    bridge->base_ = "http://phone";
    bridge->mode = 1;
    bridge->pollAsync();
    CHECK(!bridge->readyForDestruction());
    auto th = spawn();
    auto started = std::chrono::steady_clock::now();
    auto waitEnter = std::chrono::steady_clock::now();
    while (!entered.load() && msSince(waitEnter) < 2000) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(entered.load());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto stopAt = std::chrono::steady_clock::now();
    bridge->requestStop();
    auto requestStopUs = usSince(stopAt);
    CHECK(requestStopUs < 50000);
    CHECK(selfDeletes.load() == 0);
    CHECK(foreignDeletes.load() == 0);
    waitFlag(selfDeletes, 1, 2000);
    auto stopToReady = msSince(stopAt);
    auto total = msSince(started);
    const int slices = slicesDone.load();
    const int remaining = kSimulatedNetworkMs - slices * 20;
    std::printf("network cancel: slices=%d remaining_ms=%d request_stop_us=%lld stop_to_ready_ms=%lld total_ms=%lld\n",
                slices, remaining, static_cast<long long>(requestStopUs),
                static_cast<long long>(stopToReady), static_cast<long long>(total));
    CHECK(selfDeletes.load() == 1);
    CHECK(bridge->readyForDestruction());
    CHECK(!finishedNaturally.load());
    CHECK(cancelledAt.load() >= 0);
    CHECK(slices >= 1);
    CHECK(slices < kSimulatedNetworkMs / 20);
    CHECK(remaining > 2000);
    CHECK(stopToReady < 500);
    CHECK(total < 1500);
    CHECK(liveGates.load() == 0);
    CHECK(liveBufs.load() == 0);
    CHECK(gateCtors.load() == gateDtors.load());
    CHECK(bufCtors.load() == bufDtors.load());
    CHECK(gateCtors.load() >= 1);
    CHECK(gClearedUnderLock.load());
    CHECK(gWorkerLockDepth.load() == 0);
    bridge->requestStop();
    CHECK(selfDeletes.load() == 1);
    const int calls = createCalls.load();
    bridge->pollAsync();
    CHECK(createCalls.load() == calls);
    delete bridge;
    th.join();
    CHECK(foreignDeletes.load() == 0);
    CHECK(gSealed.load());
  }

  {
    resetRuntime();
    fastSuccess.store(true);
    auto* bridge = new Bridge;
    bridge->base_ = "http://phone";
    bridge->mode = 1;
    bridge->pollAsync();
    auto th = spawn();
    auto started = std::chrono::steady_clock::now();
    waitFlag(runEntered, 1, 2000);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    auto stopAt = std::chrono::steady_clock::now();
    bridge->requestStop();
    CHECK(usSince(stopAt) < 50000);
    waitFlag(selfDeletes, 1, 2000);
    auto elapsed = msSince(started);
    std::printf("normal release: request_slices=%d elapsed_ms=%lld gates=%d/%d bufs=%d/%d\n",
                slicesDone.load(), static_cast<long long>(elapsed),
                gateCtors.load(), gateDtors.load(), bufCtors.load(), bufDtors.load());
    CHECK(bridge->readyForDestruction());
    CHECK(selfDeletes.load() == 1);
    CHECK(slicesDone.load() == 0);
    CHECK(!finishedNaturally.load());
    CHECK(gateCtors.load() >= 1);
    CHECK(gateCtors.load() == gateDtors.load());
    CHECK(bufCtors.load() == bufDtors.load());
    CHECK(liveGates.load() == 0);
    CHECK(liveBufs.load() == 0);
    CHECK(elapsed < 1000);
    CHECK(foreignDeletes.load() == 0);
    delete bridge;
    th.join();
    CHECK(foreignDeletes.load() == 0);
  }

  {
    resetRuntime();
    auto* bridge = new Bridge;
    bridge->mode = 3;
    bridge->pollAsync();
    auto th = spawn();
    auto waitHold = std::chrono::steady_clock::now();
    while (!holdingMu.load() && msSince(waitHold) < 2000) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(holdingMu.load());
    auto stopAt = std::chrono::steady_clock::now();
    bridge->requestStop();
    auto requestStopUs = usSince(stopAt);
    auto readyAt = std::chrono::steady_clock::now();
    const bool readyWhileHeld = bridge->readyForDestruction();
    auto readyUs = usSince(readyAt);
    CHECK(requestStopUs < 50000);
    CHECK(readyUs < 50000);
    CHECK(!readyWhileHeld);
    CHECK(selfDeletes.load() == 0);
    CHECK(foreignDeletes.load() == 0);
    releaseHold.store(true);
    waitFlag(selfDeletes, 1, 2000);
    CHECK(selfDeletes.load() == 1);
    CHECK(bridge->readyForDestruction());
    std::printf("hold mu: request_stop_us=%lld ready_us=%lld ready_while_held=0\n",
                static_cast<long long>(requestStopUs), static_cast<long long>(readyUs));
    delete bridge;
    th.join();
    CHECK(foreignDeletes.load() == 0);
  }

  {
    resetRuntime();
    Bridge quiet;
    CHECK(quiet.delayUnlessStopped(20));
    CHECK(!quiet.stop_.load());
    resetRuntime();
    Bridge last;
    onDelay = [&] { last.requestStop(); };
    auto started = std::chrono::steady_clock::now();
    const bool proceed = last.delayUnlessStopped(20);
    auto elapsed = msSince(started);
    CHECK(!proceed);
    CHECK(last.stop_.load());
    CHECK(delayEntries.load() == 1);
    CHECK(elapsed < 200);
    std::printf("last slice: returned_false=1 delay_entries=%d elapsed_ms=%lld\n",
                delayEntries.load(), static_cast<long long>(elapsed));
  }

  {
    resetRuntime();
    auto* bridge = new Bridge;
    bridge->mode = 4;
    onDelay = [bridge] { bridge->requestStop(); };
    bridge->pollAsync();
    auto th = spawn();
    waitFlag(selfDeletes, 1, 2000);
    CHECK(selfDeletes.load() == 1);
    CHECK(followOn.load() == 0);
    CHECK(delayEntries.load() == 1);
    CHECK(bridge->readyForDestruction());
    CHECK(foreignDeletes.load() == 0);
    std::printf("last slice follow-on: requests=%d\n", followOn.load());
    delete bridge;
    th.join();
  }

  {
    resetRuntime();
    gSleepDelays.store(false);
    deferredActivities = reinterpret_cast<Activity*>(static_cast<uintptr_t>(1));
    gProvidersBusy.store(false);
    auto started = std::chrono::steady_clock::now();
    releaseM4HomeBoundaryResources();
    auto elapsed = msSince(started);
    std::printf("boundary deferred: shutdowns=%d elapsed_ms=%lld delay_entries=%d\n",
                gShutdowns.load(), static_cast<long long>(elapsed), delayEntries.load());
    CHECK(gShutdowns.load() == 0);
    CHECK(elapsed < 100);
    CHECK(delayEntries.load() == 0);

    gShutdowns.store(0);
    deferredActivities = nullptr;
    gProvidersBusy.store(false);
    releaseM4HomeBoundaryResources();
    CHECK(gShutdowns.load() == 1);

    gShutdowns.store(0);
    gProvidersBusy.store(true);
    gMillis.store(0);
    releaseM4HomeBoundaryResources();
    CHECK(gShutdowns.load() == 0);
    CHECK(gMillis.load() >= 450);

    gShutdowns.store(0);
    gProvidersBusy.store(false);
    gMillis.store(0);
    deferredActivities = reinterpret_cast<Activity*>(static_cast<uintptr_t>(1));
    gM4PendingTransientReset = true;
    existingPendingResetPath();
    CHECK(gShutdowns.load() == 0);
    CHECK(gM4PendingTransientReset);

    deferredActivities = nullptr;
    gProvidersBusy.store(false);
    existingPendingResetPath();
    std::printf("boundary clear: pending_shutdowns=%d\n", gShutdowns.load());
    CHECK(gShutdowns.load() == 1);
  }
  return 0;
}
'''
    run(code, 'actual screen bridge lifecycle / cancel, publish, native app ready')

if __name__ == '__main__':
    main()
