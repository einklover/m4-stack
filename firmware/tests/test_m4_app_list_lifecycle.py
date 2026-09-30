"""Compile the real AppListActivity lifecycle methods on the host.

The production drawer still pulls in the renderer, registry, and SD. This
harness extracts onEnter, onExit, the destructor, displayTaskLoop,
submitDirtyFrame, reload, and the cache fill those methods call, then drives
them with mutex and task stubs. No PIO, QEMU, or device.
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


def source_checks(cpp, header, home):
    assert 'displayTaskExited_{true}' in header
    assert 'displayTaskExited_{false}' not in header
    ready = function(header, 'bool readyForDestruction() const override')
    assert 'displayTaskExited_.load(std::memory_order_acquire)' in ready
    assert 'ActivityWithSubactivity::readyForDestruction()' in ready

    on_exit = function(cpp, 'void AppListActivity::onExit()')
    for banned in ('vTaskDelay', 'deleteTask', 'vSemaphoreDelete', 'vTaskDelete',
                   'for (int i = 0; i < 300', 'acceptPendingDrawer', 'pendingApps_',
                   'xSemaphoreTake'):
        assert banned not in on_exit, banned
    assert on_exit.index('childScreenOwned_.store(true') < on_exit.index(
        'exitDisplayTask_.store(true') < on_exit.index('ActivityWithSubactivity::onExit()')

    dtor = function(cpp, 'AppListActivity::~AppListActivity()')
    for banned in ('vTaskDelay', 'deleteTask', 'vTaskDelete'):
        assert banned not in dtor, dtor
    assert dtor.index('displayTaskExited_.load(std::memory_order_acquire)') < dtor.index(
        'pendingApps_.clear()') < dtor.index('vSemaphoreDelete(renderingMutex_)')
    assert 'pendingItems_.clear()' in dtor
    assert 'vSemaphoreDelete(reloadLock_)' in dtor

    enter = function(cpp, 'void AppListActivity::onEnter()')
    mutex_fail = enter.index('failed to create display mutex')
    reload_at = enter.index('reload();')
    false_at = enter.index('displayTaskExited_.store(false')
    create_at = enter.index('M4Psram::createTask')
    assert mutex_fail < enter.index('return;') < reload_at
    assert false_at < create_at
    assert enter.index('displayTaskExited_.store(true', create_at) > create_at
    assert enter.index('acceptPendingDrawer()', reload_at) > reload_at

    assert 'std::atomic<bool> verifyDrawerCache_' in header
    assert 'std::atomic<bool> showedDrawer_' in header
    assert 'std::vector<M4xInstalledApp> pendingApps_;' in header
    assert 'std::vector<DrawerItem> pendingItems_;' in header
    assert 'std::atomic<bool> pendingReady_{false};' in header
    assert 'void acceptPendingDrawer();' in header
    assert 'void discardPendingDrawer();' in header
    assert 'reloadChanged_' not in header
    assert 'reloadPreserveDialog_' not in header
    assert 'void submitDirtyFrame();' in header
    assert 'bool reload(bool preserveDialog = false);' in header

    worker = function(cpp, 'void AppListActivity::displayTaskLoop()')
    for banned in ('render(', 'M4RenderGuard', 'setMask', 'snapshot.items',
                   'subActivity', 'clearScreen', 'rememberDrawer', 'updateRequired_',
                   'acceptPendingDrawer', 'apps_ =', 'items_ =', 'selectedIndex_ =',
                   'mode_ = 0'):
        assert banned not in worker, banned
    assert 'pendingReady_' in worker
    take = worker.index('xSemaphoreTake(renderingMutex_')
    assert take < worker.index('mode_ ==', take) < worker.index(
        'xSemaphoreGive(renderingMutex_)', take)

    store = worker.index('displayTaskExited_.store(true, std::memory_order_release)')
    assert worker.index('discardPendingDrawer()') < store
    assert worker.index('displayTaskHandle_ = nullptr') < store
    clear_verify = worker.index('verifyDrawerCache_.store(false')
    assert 'pendingReady_' in worker[clear_verify - 220:clear_verify]
    after = worker.index(';', store) + 1
    tail_end = worker.index('M4Psram::deleteTask(nullptr);') + len('M4Psram::deleteTask(nullptr);')
    tail = worker[after:tail_end]
    for token in ('displayTaskHandle_', 'renderingMutex_', 'reloadLock_', 'snapshot_',
                  'items_', 'apps_', 'selectedIndex_', 'this->'):
        assert token not in tail, (token, tail)
    assert 'M4Psram::deleteTask(nullptr)' in tail

    submit = function(cpp, 'void AppListActivity::submitDirtyFrame()')
    snap = submit.index('snapshot.items = items_')
    give = submit.index('xSemaphoreGive(renderingMutex_)', snap)
    guard = submit.index('M4RenderGuard')
    assert snap < give < guard
    assert 'AppListFrameSnapshot snapshot;' not in submit
    assert 'snapshot_ = snapshot' not in submit
    assert 'AppListFrameSnapshot& snapshot = snapshot_;' in submit
    assert snap < submit.index('updateRequired_ = false;') < give
    assert snap < submit.index('setMask') < give < guard
    assert submit.index('render();') > guard
    submit.index('childScreenOwned_', guard)
    assert submit.index('xSemaphoreTake(renderingMutex_', guard) > guard
    assert 'subActivity' in submit

    ui = function(cpp, 'void AppListActivity::loop()')
    sub = ui.index('if (subActivity)')
    ret = ui.index('return;', sub)
    accept_at = ui.index('acceptPendingDrawer()', ret)
    called = ui.index('submitDirtyFrame()', accept_at)
    assert sub < ret < accept_at < called < ui.index('mappedInput', called)
    pos = 0
    while True:
        at = ui.find('mode_ = 1', pos)
        if at < 0:
            break
        window = ui[max(0, at - 400):at]
        assert window.rfind('xSemaphoreTake(renderingMutex_') > window.rfind(
            'xSemaphoreGive(renderingMutex_)')
        pos = at + 1

    reload = function(cpp, 'bool AppListActivity::reload(')
    for banned in ('setMask', 'M4RenderGuard', 'render(', 'rememberDrawer',
                   'apps_ = apps', 'items_ = std::move(items)', 'mode_ = 0',
                   'selectedIndex_ =', 'updateRequired_', 'acceptPendingDrawer'):
        assert banned not in reload, banned
    assert 'pendingApps_ = std::move(apps)' in reload
    assert 'pendingItems_ = std::move(items)' in reload
    assert 'pendingReady_.store(true' in reload
    load_at = reload.index('M4xRegistry::load()')
    assert load_at < reload.index('exitDisplayTask_', load_at) < reload.index('addBuiltin', load_at)
    publish_lock = reload.rindex('xSemaphoreTake(renderingMutex_')
    assert publish_lock < reload.index('exitDisplayTask_', publish_lock) < reload.index(
        'pendingApps_ = std::move(apps)')

    accept = function(cpp, 'void AppListActivity::acceptPendingDrawer()')
    assert 'apps_ = std::move(pendingApps_)' in accept
    assert 'items_ = std::move(pendingItems_)' in accept
    assert 'pendingReady_.store(false' in accept
    assert 'rememberDrawer' in accept
    for banned in ('setMask', 'render(', 'M4RenderGuard'):
        assert banned not in accept, banned
    assert accept.index('discardPendingDrawer()') < accept.index('xSemaphoreTake(renderingMutex_')
    assert accept.index('xSemaphoreGive(renderingMutex_)') < accept.index('M4ReturnCache::rememberDrawer')

    discard = function(cpp, 'void AppListActivity::discardPendingDrawer()')
    for banned in ('rememberDrawer', 'setMask', 'render(', 'M4RenderGuard'):
        assert banned not in discard, banned
    assert discard.index('xSemaphoreTake(renderingMutex_') < discard.index(
        'pendingApps_.clear()') < discard.index('pendingItems_.clear()') < discard.index(
        'xSemaphoreGive(renderingMutex_)')

    uninstall = function(cpp, 'void AppListActivity::uninstallSelected()')
    uninstall_reload = uninstall.index('reload();')
    assert uninstall.index('acceptPendingDrawer()', uninstall_reload) > uninstall_reload

    assert cpp.count('M4Psram::deleteTask(') == 1
    assert 'deleteTask(displayTaskHandle_' not in cpp
    assert 'vTaskDelete' not in cpp

    home_loop_at = home.index(
        'void HomeActivity::displayTaskLoop(const std::shared_ptr<BackendContext>& ctx)')
    home_loop = home[home_loop_at:home.index('#else', home_loop_at)]
    assert 'gM4RenderMutex' not in home_loop
    assert 'M4RenderGuard' not in home_loop
    assert home_loop.index('xSemaphoreTake(renderingMutex') < home_loop.index('render(ctx)')
    render_at = home.index(
        'void HomeActivity::render(const std::shared_ptr<BackendContext>& ctx)')
    render_window = home[render_at:render_at + 900]
    assert 'renderSnapshotScene(ctx)' in render_window
    assert 'gM4RenderMutex' not in render_window
    assert 'M4RenderGuard' not in render_window


def run(code):
    with tempfile.TemporaryDirectory(prefix='m4-app-list-') as tmp:
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
            raise SystemExit(f'app list lifecycle failed ({ran.returncode})')


def main():
    cpp = (SRC / 'activities/apps/AppListActivity.cpp').read_text()
    header = (SRC / 'activities/apps/AppListActivity.h').read_text()
    home = (SRC / 'activities/home/HomeActivity.cpp').read_text()
    source_checks(cpp, header, home)
    sigs = [
        'void AppListActivity::taskTrampoline(void* param)',
        'void AppListActivity::displayTaskLoop()',
        'bool sameInstalledApp(const M4xInstalledApp& a, const M4xInstalledApp& b)',
        'bool AppListActivity::sameDrawer(',
        'bool AppListActivity::applyCachedDrawer()',
        'void AppListActivity::submitDirtyFrame()',
        'bool AppListActivity::reload(',
        'void AppListActivity::onEnter()',
        'void AppListActivity::onExit()',
        'AppListActivity::~AppListActivity()',
        'void AppListActivity::discardPendingDrawer()',
        'void AppListActivity::acceptPendingDrawer()',
    ]
    bodies = '\n'.join(function(cpp, sig).replace('std::vector', 'TrackedVector') for sig in sigs)
    ready = function(header, 'bool readyForDestruction() const override')
    code = r'''
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct TaskExit {};

static std::atomic<int> gCopyAssigns{0};
static std::atomic<int> gCopyCtors{0};
static std::atomic<int> gCopiesAtRender{-1};
static std::atomic<int> gCtorsAtRender{-1};
static std::atomic<int> gItemsAtRender{-1};
static std::atomic<int> gAppsAtRender{-1};
static std::atomic<int> gRenders{0};
static std::atomic<bool> gBlockAssign{false};
static std::atomic<bool> gAssignHolding{false};
static std::atomic<bool> gBlockLoad{false};
static std::atomic<bool> gLoadHolding{false};
static std::atomic<int> gFailMutex{0};
static std::atomic<bool> gFailCreate{false};
static std::atomic<bool> gCacheHit{false};
static std::atomic<int> gCreateOrdinal{0};
static std::atomic<int> gCreateCalls{0};
static std::atomic<int> gForeignDeletes{0};
static std::atomic<int> gSelfDeletes{0};
static std::atomic<int> gSemDeletes{0};
static std::atomic<int> gSemLive{0};
static std::atomic<int> gLocksHeld{0};
static std::atomic<int> gEnsure{0};
static std::atomic<bool> gDeleteOnExitLog{false};
static std::atomic<bool> gDeletedInPrintf{false};
static std::atomic<int> gFooterSets{0};
static std::atomic<int> gHomeDraws{0};
static std::atomic<int> gSubmitGuards{0};
static std::atomic<int> gLoadGeneration{0};
static std::atomic<int> gRemember{0};

struct HostSem {
  std::timed_mutex mu;
};
using SemaphoreHandle_t = HostSem*;
struct HostTask {
  std::thread thr;
};
using TaskHandle_t = HostTask*;

static std::mutex gTaskMu;
static std::vector<HostTask*> gTasks;

class AppListActivity;
static std::atomic<AppListActivity*> gVictim{nullptr};
void deleteVictimIfExitLog(const char* fmt);

struct SerialType {
  void printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_end(ap);
    deleteVictimIfExitLog(fmt);
  }
};
static SerialType Serial;

static unsigned long millis() { return 0; }
static unsigned uxTaskGetStackHighWaterMark(void*) { return 128; }

constexpr int pdTRUE = 1;
constexpr int pdFALSE = 0;
constexpr int pdPASS = 1;
constexpr int pdFAIL = 0;
constexpr uint32_t portMAX_DELAY = 0xFFFFFFFFu;
constexpr int portTICK_PERIOD_MS = 1;
#define pdMS_TO_TICKS(ms) (ms)

static SemaphoreHandle_t xSemaphoreCreateMutex() {
  const int n = gCreateOrdinal.fetch_add(1);
  const int mode = gFailMutex.load();
  if (mode == 3 || (mode == 1 && n == 0) || (mode == 2 && n == 1)) return nullptr;
  gSemLive.fetch_add(1);
  return new HostSem;
}

static int xSemaphoreTake(SemaphoreHandle_t sem, uint32_t ticks) {
  if (!sem) return pdFALSE;
  bool locked = false;
  if (ticks == portMAX_DELAY) {
    sem->mu.lock();
    locked = true;
  } else {
    locked = sem->mu.try_lock_for(std::chrono::milliseconds(ticks));
  }
  if (!locked) return pdFALSE;
  gLocksHeld.fetch_add(1);
  return pdTRUE;
}

static void xSemaphoreGive(SemaphoreHandle_t sem) {
  gLocksHeld.fetch_sub(1);
  sem->mu.unlock();
}

static void vSemaphoreDelete(SemaphoreHandle_t sem) {
  gSemDeletes.fetch_add(1);
  if (!sem) {
    std::fprintf(stderr, "deleted a null semaphore\n");
    std::abort();
  }
  delete sem;
  gSemLive.fetch_sub(1);
}

static void vTaskDelay(uint32_t ticks) {
  if (ticks >= 20 && ticks < 100000) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ticks));
  } else {
    std::this_thread::yield();
  }
}

template <typename T>
struct TrackedVector {
  std::vector<T> v;
  TrackedVector() = default;
  TrackedVector(const TrackedVector& o) : v(o.v) { gCopyCtors.fetch_add(1); }
  TrackedVector(TrackedVector&& o) noexcept : v(std::move(o.v)) {}
  TrackedVector& operator=(const TrackedVector& o) {
    if (this != &o) {
      gCopyAssigns.fetch_add(1);
      if (gBlockAssign.load(std::memory_order_acquire)) {
        gAssignHolding.store(true, std::memory_order_release);
        while (gBlockAssign.load(std::memory_order_acquire)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
      }
      v = o.v;
    }
    return *this;
  }
  TrackedVector& operator=(TrackedVector&& o) noexcept {
    // Block only the publish that already holds reloadLock_ and renderingMutex_.
    // Earlier moves (the load result, the cache fill) hold one lock or none.
    if (gBlockAssign.load(std::memory_order_acquire) &&
        gLocksHeld.load(std::memory_order_acquire) >= 2) {
      gAssignHolding.store(true, std::memory_order_release);
      while (gBlockAssign.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }
    v = std::move(o.v);
    return *this;
  }
  void push_back(const T& x) { v.push_back(x); }
  void push_back(T&& x) { v.push_back(std::move(x)); }
  void reserve(size_t n) { v.reserve(n); }
  void clear() { v.clear(); }
  void resize(size_t n) { v.resize(n); }
  size_t size() const { return v.size(); }
  bool empty() const { return v.empty(); }
  T& operator[](size_t i) { return v[i]; }
  const T& operator[](size_t i) const { return v[i]; }
  T* data() { return v.data(); }
  const T* data() const { return v.data(); }
  auto begin() { return v.begin(); }
  auto end() { return v.end(); }
  auto begin() const { return v.begin(); }
  auto end() const { return v.end(); }
};

enum class M4xRuntimeKind : uint8_t { Lua = 0, Native = 1 };
struct M4xInstalledApp {
  std::string id;
  std::string name;
  std::string version;
  int versionCode = 0;
  std::string path;
  M4xRuntimeKind runtime = M4xRuntimeKind::Lua;
  std::string entry;
  std::string provider;
  std::string icon;
  std::vector<std::string> permissions;
  std::vector<std::string> files;
  uint32_t installedAt = 0;
};

enum UIIcon {
  Folder = 1,
  WifiTransfer,
  Library,
  Recent,
  Hotspot,
  Transfer,
  Cog,
  Book,
  Wifi,
  Settings
};

enum class Str {
  kFileManager,
  kWifiTransfer,
  kReadingHistory,
  kOPDSBrowser,
  kJianGuoDisk,
  kDataCapsule,
  kBookmarkNotes,
  kNetworkManage,
  kSystemSettings
};
static const char* L(Str) { return "label"; }

struct SettingsStub {
  char opdsServerUrl[8]{};
  char jgUsername[8]{};
  char dcUsername[8]{};
};
static SettingsStub SETTINGS;

namespace BookmarkStore {
static bool hasAnyBookmarks() { return false; }
}
namespace M4xInstaller {
static void ensureLayout() { gEnsure.fetch_add(1); }
}
namespace HomeScene {
constexpr int kHomeAppIconW = 62;
constexpr int kHomeAppIconH = 64;
constexpr int kHomeAppIconStride = 8;
constexpr size_t kHomeAppIconBytes = 512;
}
namespace HomeSceneAssetDecoder {
static std::string resolveAppIconPath(const std::string&, const std::string&) { return {}; }
static bool decodeBmpFileTo1Bit(const char*, uint8_t*, int, int, int) { return false; }
}
namespace M4FooterTouchPolicy {
static void setMask(uint8_t) { gFooterSets.fetch_add(1); }
}
namespace M4ReturnCache {
struct DrawerItem {
  bool plugin = false;
  uint8_t builtin = 0;
  int appIndex = -1;
  std::string id;
  std::string label;
  int icon = 0;
  std::vector<uint8_t> pluginIcon;
};
static bool copyDrawerIcon(const std::string&, int, const std::string&, const std::string&, uint32_t,
                           std::vector<uint8_t>&) {
  return false;
}
static bool restoreDrawer(TrackedVector<M4xInstalledApp>& apps, TrackedVector<DrawerItem>& items) {
  if (!gCacheHit.load()) return false;
  for (int i = 0; i < 2; ++i) {
    M4xInstalledApp app;
    app.id = "plugin-" + std::to_string(i);
    app.name = "Plugin";
    app.version = "1";
    app.versionCode = i + 1;
    apps.push_back(std::move(app));
  }
  for (int i = 0; i < 4; ++i) {
    DrawerItem item;
    item.id = "item-" + std::to_string(i);
    item.label = "Item";
    item.icon = Library;
    items.push_back(std::move(item));
  }
  return true;
}
static void rememberDrawer(const TrackedVector<M4xInstalledApp>&, const TrackedVector<DrawerItem>&) {
  gRemember.fetch_add(1);
}
}
namespace M4xRegistry {
static TrackedVector<M4xInstalledApp> load() {
  if (gBlockLoad.load(std::memory_order_acquire)) {
    gLoadHolding.store(true, std::memory_order_release);
    while (gBlockLoad.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  TrackedVector<M4xInstalledApp> apps;
  if (gLoadGeneration.load(std::memory_order_acquire) > 0) {
    M4xInstalledApp app;
    app.id = "changed";
    app.name = "Changed";
    app.version = "2";
    app.versionCode = 9;
    apps.push_back(std::move(app));
  }
  return apps;
}
}

class M4RenderGuard final {
 public:
  explicit M4RenderGuard(SemaphoreHandle_t mutex, uint32_t wait = pdMS_TO_TICKS(100)) : mutex_(mutex) {
    if (mutex_ != nullptr) owns_ = (xSemaphoreTake(mutex_, wait) == pdTRUE);
    if (owns_) gSubmitGuards.fetch_add(1);
  }
  ~M4RenderGuard() { unlock(); }
  M4RenderGuard(const M4RenderGuard&) = delete;
  M4RenderGuard& operator=(const M4RenderGuard&) = delete;
  void unlock() {
    if (owns_) {
      xSemaphoreGive(mutex_);
      owns_ = false;
    }
  }
  bool owns() const { return owns_; }

 private:
  SemaphoreHandle_t mutex_ = nullptr;
  bool owns_ = false;
};

static SemaphoreHandle_t gM4RenderMutex = nullptr;

namespace M4Psram {
static int createTask(void (*fn)(void*), const char*, uint32_t, void* arg, int, TaskHandle_t* out) {
  gCreateCalls.fetch_add(1);
  if (gFailCreate.load()) {
    if (out) *out = nullptr;
    return pdFAIL;
  }
  auto* task = new HostTask;
  {
    std::lock_guard<std::mutex> lock(gTaskMu);
    gTasks.push_back(task);
  }
  task->thr = std::thread([fn, arg] {
    try {
      fn(arg);
    } catch (const TaskExit&) {
    }
  });
  if (out) *out = task;
  return pdPASS;
}
static void deleteTask(TaskHandle_t handle) {
  if (handle != nullptr) {
    gForeignDeletes.fetch_add(1);
    return;
  }
  if (gLocksHeld.load() != 0) {
    std::fprintf(stderr, "self-delete while locks held %d\n", gLocksHeld.load());
    std::abort();
  }
  gSelfDeletes.fetch_add(1);
  throw TaskExit();
}
}

class ActivityWithSubactivity {
 public:
  virtual ~ActivityWithSubactivity() = default;
  virtual void onEnter() {}
  virtual void onExit() {}
  virtual bool readyForDestruction() const { return true; }
  std::unique_ptr<int> subActivity;
};

class AppListActivity : public ActivityWithSubactivity {
 public:
  enum class BuiltinAction : uint8_t {
    FileManager,
    FileTransfer,
    AppStore,
    RecentBooks,
    Opds,
    JianGuo,
    DataCapsule,
    BookmarkNotes,
    Network,
    Settings,
  };
  struct DrawerItem {
    bool plugin = false;
    BuiltinAction builtin = BuiltinAction::Settings;
    int appIndex = -1;
    std::string id;
    std::string label;
    UIIcon icon = UIIcon::Library;
    std::vector<uint8_t> pluginIcon;
  };
  struct AppListFrameSnapshot {
    int selectedIndex = 0;
    int mode = 0;
    bool uninstallClearData = true;
    TrackedVector<DrawerItem> items;
    TrackedVector<M4xInstalledApp> apps;
  };

  TrackedVector<M4xInstalledApp> apps_;
  TrackedVector<DrawerItem> items_;
  int selectedIndex_ = 0;
  std::atomic<bool> updateRequired_{false};
  int mode_ = 0;
  bool uninstallClearData_ = true;
  TaskHandle_t displayTaskHandle_ = nullptr;
  SemaphoreHandle_t renderingMutex_ = nullptr;
  std::atomic<bool> exitDisplayTask_{false};
  std::atomic<bool> displayTaskExited_{true};
  std::atomic<bool> childScreenOwned_{false};
  std::atomic<bool> verifyDrawerCache_{false};
  std::atomic<bool> showedDrawer_{false};
  TrackedVector<M4xInstalledApp> pendingApps_;
  TrackedVector<DrawerItem> pendingItems_;
  std::atomic<bool> pendingReady_{false};
  SemaphoreHandle_t reloadLock_ = nullptr;
  AppListFrameSnapshot snapshot_;

  __READY__

  void onEnter();
  void onExit();
  ~AppListActivity() override;
  static void taskTrampoline(void* param);
  void displayTaskLoop();
  void submitDirtyFrame();
  void acceptPendingDrawer();
  void discardPendingDrawer();
  bool reload(bool preserveDialog = false);
  bool applyCachedDrawer();
  static bool sameDrawer(const TrackedVector<M4xInstalledApp>&, const TrackedVector<DrawerItem>&,
                         const TrackedVector<M4xInstalledApp>&, const TrackedVector<DrawerItem>&);
  uint8_t touchFooterButtonsMask() const { return 1; }
  void render() const {
    if (gRenders.load() == 0) {
      gCopiesAtRender.store(gCopyAssigns.load());
      gCtorsAtRender.store(gCopyCtors.load());
      gItemsAtRender.store(static_cast<int>(snapshot_.items.size()));
      gAppsAtRender.store(static_cast<int>(snapshot_.apps.size()));
    }
    gRenders.fetch_add(1);
  }
};

void deleteVictimIfExitLog(const char* fmt) {
  if (!gDeleteOnExitLog.load() || fmt == nullptr || std::strstr(fmt, "applist-display-task-exit") == nullptr) {
    return;
  }
  AppListActivity* victim = gVictim.exchange(nullptr);
  if (!victim) return;
  if (gLocksHeld.load() != 0) {
    std::fprintf(stderr, "publication destroy while locks held %d\n", gLocksHeld.load());
    std::abort();
  }
  delete victim;
  gDeletedInPrintf.store(true);
}

__BODIES__

static void require(bool cond, const char* what) {
  if (!cond) {
    std::fprintf(stderr, "REQUIRE %s\n", what);
    std::abort();
  }
}

static void waitFor(const std::atomic<bool>& flag, const char* what) {
  const auto start = std::chrono::steady_clock::now();
  while (!flag.load(std::memory_order_acquire)) {
    if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
      std::fprintf(stderr, "timeout waiting for %s\n", what);
      std::abort();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

static void joinTasks() {
  std::vector<HostTask*> tasks;
  {
    std::lock_guard<std::mutex> lock(gTaskMu);
    tasks.swap(gTasks);
  }
  for (HostTask* task : tasks) {
    if (task->thr.joinable()) task->thr.join();
    delete task;
  }
}

static void resetCounters() {
  gCopyAssigns.store(0);
  gCopyCtors.store(0);
  gCopiesAtRender.store(-1);
  gCtorsAtRender.store(-1);
  gItemsAtRender.store(-1);
  gAppsAtRender.store(-1);
  gRenders.store(0);
  gBlockAssign.store(false);
  gAssignHolding.store(false);
  gBlockLoad.store(false);
  gLoadHolding.store(false);
  gFailMutex.store(0);
  gFailCreate.store(false);
  gCacheHit.store(false);
  gCreateOrdinal.store(0);
  gCreateCalls.store(0);
  gForeignDeletes.store(0);
  gSelfDeletes.store(0);
  gSemDeletes.store(0);
  gEnsure.store(0);
  gDeleteOnExitLog.store(false);
  gDeletedInPrintf.store(false);
  gFooterSets.store(0);
  gHomeDraws.store(0);
  gSubmitGuards.store(0);
  gLoadGeneration.store(0);
  gRemember.store(0);
  gVictim.store(nullptr);
  require(gSemLive.load() == 0, "semaphore leak across scenarios");
  require(gLocksHeld.load() == 0, "lock leak across scenarios");
  require(gTasks.empty(), "task leak across scenarios");
}

static long onExitMs(AppListActivity* activity) {
  const auto start = std::chrono::steady_clock::now();
  activity->onExit();
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}

static void finishNaturally(AppListActivity* activity) {
  const auto start = std::chrono::steady_clock::now();
  while (!activity->displayTaskExited_.load(std::memory_order_acquire)) {
    if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
      std::fprintf(stderr, "timeout waiting for natural publish\n");
      std::abort();
    }
    std::this_thread::yield();
  }
  joinTasks();
  require(activity->readyForDestruction(), "ready after natural publish");
  require(gForeignDeletes.load() == 0, "foreign delete after publish");
  delete activity;
  require(gSemDeletes.load() == 2, "destructor deletes both mutexes");
  require(gSemLive.load() == 0, "both mutexes released");
  require(gSelfDeletes.load() == 1, "owner self-deletes once");
  require(gLocksHeld.load() == 0, "locks clear after destroy");
}

static void simulateHomeDraw() { gHomeDraws.fetch_add(1); }

static void testDisplayHold() {
  resetCounters();
  gCacheHit.store(true);
  gBlockLoad.store(true);
  auto* activity = new AppListActivity;
  activity->onEnter();
  activity->submitDirtyFrame();
  require(gRenders.load() == 1, "main loop painted the first dirty frame");
  waitFor(gLoadHolding, "reload reached the blocked load");
  gBlockAssign.store(true);
  gBlockLoad.store(false);
  waitFor(gAssignHolding, "reload publish assign");
  require(!activity->renderingMutex_->mu.try_lock(), "rendering mutex stayed held");
  require(!activity->reloadLock_->mu.try_lock(), "reload mutex stayed held");
  const long elapsed = onExitMs(activity);
  const bool ready = activity->readyForDestruction();
  std::printf("display-hold: on_exit_ms=%ld ready=%d foreign_deletes=%d sem_deletes=%d locks=%d\n",
              elapsed, ready ? 1 : 0, gForeignDeletes.load(), gSemDeletes.load(), gLocksHeld.load());
  require(elapsed < 200, "onExit returned immediately while display lock held");
  require(!ready, "not ready while display owner holds the mutex");
  require(gForeignDeletes.load() == 0, "display task not deleted");
  require(gSemDeletes.load() == 0, "mutexes not deleted while display owner runs");
  require(activity->displayTaskHandle_ != nullptr, "task handle still live during hold");
  require(gLocksHeld.load() == 2, "reload and rendering mutexes both held");
  gBlockAssign.store(false);
  finishNaturally(activity);
  std::printf("display-release: ready=1 sem_deletes=%d self_deletes=%d\n", 2, 1);
}

static void testReloadHold() {
  resetCounters();
  gCacheHit.store(true);
  gBlockLoad.store(true);
  auto* activity = new AppListActivity;
  activity->onEnter();
  activity->submitDirtyFrame();
  require(gRenders.load() == 1, "main loop painted before the blocked reload");
  require(gSubmitGuards.load() == 1, "one submit guard for the main-loop frame");
  const int footer = gFooterSets.load();
  const int renders = gRenders.load();
  const int guards = gSubmitGuards.load();
  waitFor(gLoadHolding, "reload lock");
  require(!activity->reloadLock_->mu.try_lock(), "reload mutex stayed held");
  const long elapsed = onExitMs(activity);
  const bool ready = activity->readyForDestruction();
  std::printf("cross-page: on_exit_ms=%ld ready=%d foreign_deletes=%d sem_deletes=%d locks=%d\n",
              elapsed, ready ? 1 : 0, gForeignDeletes.load(), gSemDeletes.load(), gLocksHeld.load());
  require(elapsed < 200, "onExit returned immediately while reload lock held");
  require(!ready, "not ready while reload owner holds the mutex");
  require(gForeignDeletes.load() == 0, "task not deleted during reload hold");
  require(gSemDeletes.load() == 0, "mutexes not deleted during reload hold");
  require(activity->displayTaskHandle_ != nullptr, "task handle still live during reload hold");
  simulateHomeDraw();
  gBlockLoad.store(false);
  const auto start = std::chrono::steady_clock::now();
  while (!activity->displayTaskExited_.load(std::memory_order_acquire)) {
    if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
      std::fprintf(stderr, "timeout waiting for worker after home draw\n");
      std::abort();
    }
    std::this_thread::yield();
  }
  std::printf("cross-page-resume: renders=%d footer=%d guards=%d home_draws=%d\n", gRenders.load(),
              gFooterSets.load(), gSubmitGuards.load(), gHomeDraws.load());
  require(gRenders.load() == renders, "resumed worker did not draw");
  require(gFooterSets.load() == footer, "resumed worker did not write the global footer");
  require(gSubmitGuards.load() == guards, "resumed worker did not submit");
  require(gHomeDraws.load() == 1, "simulated home draw ran while the worker was blocked");
  finishNaturally(activity);
  std::printf("cross-page-release: ready=1 sem_deletes=%d self_deletes=%d\n", 2, 1);
}

static void testNeverEnter() {
  resetCounters();
  auto* activity = new AppListActivity;
  require(activity->readyForDestruction(), "never-entered activity is destructible");
  require(activity->displayTaskExited_.load(), "default exit flag is published");
  delete activity;
  std::printf("never-enter: ready=1 sem_deletes=%d sem_live=%d\n", gSemDeletes.load(), gSemLive.load());
  require(gSemDeletes.load() == 0, "never-enter deletes nothing");
  require(gCreateCalls.load() == 0, "never-enter creates no task");
  require(gEnsure.load() == 0, "never-enter does not reload");
}

static void testMutexFail(int mode, int expectDeletes, const char* label) {
  resetCounters();
  gFailMutex.store(mode);
  gCacheHit.store(false);
  auto* activity = new AppListActivity;
  activity->onEnter();
  require(activity->readyForDestruction(), label);
  require(gCreateCalls.load() == 0, "mutex failure does not create a task");
  require(gEnsure.load() == 0, "mutex failure does not reload");
  require(gTasks.empty(), "mutex failure starts no worker");
  delete activity;
  std::printf("%s: ready=1 reloads=%d tasks=%d sem_deletes=%d sem_live=%d\n", label, gEnsure.load(),
              gCreateCalls.load(), gSemDeletes.load(), gSemLive.load());
  require(gSemDeletes.load() == expectDeletes, "mutex failure destructor count");
  require(gSemLive.load() == 0, "mutex failure leaks a semaphore");
}

static void testTaskFail() {
  resetCounters();
  gFailCreate.store(true);
  gCacheHit.store(false);
  auto* activity = new AppListActivity;
  activity->onEnter();
  require(activity->readyForDestruction(), "task create failure is destructible");
  require(activity->displayTaskExited_.load(), "create failure republishes exit");
  require(activity->displayTaskHandle_ == nullptr, "create failure leaves no handle");
  require(gEnsure.load() >= 1, "task failure may still have reloaded");
  require(gTasks.empty(), "task failure starts no worker");
  delete activity;
  std::printf("task-fail: ready=1 reloads=%d tasks=%d sem_deletes=%d self_deletes=%d\n", gEnsure.load(),
              gCreateCalls.load(), gSemDeletes.load(), gSelfDeletes.load());
  require(gCreateCalls.load() == 1, "create was attempted once");
  require(gSemDeletes.load() == 2, "task failure deletes both mutexes");
  require(gSelfDeletes.load() == 0, "task failure does not self-delete");
  require(gForeignDeletes.load() == 0, "task failure does not foreign-delete");
  require(gSemLive.load() == 0, "task failure leaks a semaphore");
}

static void testPublishUaf() {
  resetCounters();
  gCacheHit.store(true);
  auto* activity = new AppListActivity;
  activity->onEnter();
  gVictim.store(activity);
  gDeleteOnExitLog.store(true);
  const long elapsed = onExitMs(activity);
  require(elapsed < 200, "publish onExit does not wait");
  joinTasks();
  std::printf("publish-uaf: on_exit_ms=%ld deleted_in_printf=%d self_deletes=%d sem_deletes=%d foreign_deletes=%d\n",
              elapsed, gDeletedInPrintf.load() ? 1 : 0, gSelfDeletes.load(), gSemDeletes.load(),
              gForeignDeletes.load());
  require(gDeletedInPrintf.load(), "publication printf destroyed the activity");
  require(gSelfDeletes.load() == 1, "self-delete follows publication");
  require(gSemDeletes.load() == 2, "publication destroy released both mutexes");
  require(gForeignDeletes.load() == 0, "publication does not foreign-delete");
  require(gSemLive.load() == 0, "publication leaks a semaphore");
  require(gLocksHeld.load() == 0, "publication left a lock");
}

static void testDirtyFrame() {
  resetCounters();
  gCacheHit.store(true);
  auto* activity = new AppListActivity;
  activity->onEnter();
  activity->verifyDrawerCache_.store(false, std::memory_order_release);
  activity->submitDirtyFrame();
  std::printf("dirty-frame: copy_assigns=%d copy_ctors=%d items=%d apps=%d\n", gCopiesAtRender.load(),
              gCtorsAtRender.load(), gItemsAtRender.load(), gAppsAtRender.load());
  require(gRenders.load() == 1, "dirty frame rendered on the main-loop submit");
  require(gCopiesAtRender.load() == 2, "one vector copy-assign for items and one for apps");
  require(gCtorsAtRender.load() == 0, "no extra snapshot vector copy-construct");
  require(gItemsAtRender.load() == 4, "items assign delivered the full list");
  require(gAppsAtRender.load() == 2, "apps assign delivered the full list");
  activity->onExit();
  finishNaturally(activity);
}

static void testFrames() {
  resetCounters();
  gCacheHit.store(true);
  auto* activity = new AppListActivity;
  activity->onEnter();
  activity->verifyDrawerCache_.store(false, std::memory_order_release);
  activity->submitDirtyFrame();
  require(gRenders.load() == 1, "initial dirty frame rendered once");
  require(gCopiesAtRender.load() == 2, "initial frame copied both vectors once");
  require(gCtorsAtRender.load() == 0, "initial frame did not copy-construct a vector");
  require(gItemsAtRender.load() == 4 && gAppsAtRender.load() == 2, "initial frame used the cache");
  activity->submitDirtyFrame();
  require(gRenders.load() == 1, "clean frame did not render again");

  activity->selectedIndex_ = 3;
  activity->updateRequired_.store(true, std::memory_order_release);
  activity->submitDirtyFrame();
  require(gRenders.load() == 2, "selection dirty frame rendered once");
  require(activity->snapshot_.selectedIndex == 3, "selection frame kept the new index");
  activity->submitDirtyFrame();
  require(gRenders.load() == 2, "selection frame was not repeated");

  const int footer = gFooterSets.load();
  const int renders = gRenders.load();
  const int guards = gSubmitGuards.load();
  gLoadGeneration.store(1, std::memory_order_release);
  activity->verifyDrawerCache_.store(true, std::memory_order_release);
  waitFor(activity->pendingReady_, "worker published a pending drawer");
  require(activity->items_.size() == 4, "pending publish left the UI items alone");
  require(activity->apps_.size() == 2, "pending publish left the UI apps alone");
  require(activity->selectedIndex_ == 3, "pending publish left the selection alone");
  require(!activity->updateRequired_.load(), "worker publish did not arm a frame");
  activity->submitDirtyFrame();
  require(gRenders.load() == renders, "unconsumed pending did not draw");
  require(gFooterSets.load() == footer, "worker reload did not write the footer");
  require(gSubmitGuards.load() == guards, "worker reload did not submit");
  activity->acceptPendingDrawer();
  require(activity->apps_.size() == 1, "accept replaced the app list");
  require(activity->apps_[0].id == "changed", "accept published the new app");
  require(activity->items_.size() == 7, "accept published builtins plus the plugin");
  require(activity->selectedIndex_ == 3, "in-range selection survived accept");
  require(!activity->pendingReady_.load(), "accept consumed pending");
  activity->submitDirtyFrame();
  require(gRenders.load() == renders + 1, "cache update rendered once");
  require(activity->snapshot_.apps.size() == 1, "cache frame published the reloaded apps");
  require(activity->snapshot_.apps[0].id == "changed", "cache frame published the new app");
  require(activity->snapshot_.selectedIndex == 3, "cache frame kept the selection");
  require(activity->snapshot_.items.size() == activity->items_.size(), "cache frame copied the new items");
  activity->submitDirtyFrame();
  require(gRenders.load() == renders + 1, "cache frame was not repeated");
  std::printf("frames: initial=1 selection=2 cache=%d apps=%zu id=%s\n", gRenders.load(),
              activity->snapshot_.apps.size(), activity->snapshot_.apps[0].id.c_str());
  activity->onExit();
  finishNaturally(activity);
}

static void testPendingHandoff() {
  resetCounters();
  gCacheHit.store(true);
  auto* activity = new AppListActivity;
  activity->onEnter();
  activity->verifyDrawerCache_.store(false, std::memory_order_release);
  require(!activity->pendingReady_.load(), "cache hit did not leave a pending drawer");
  activity->submitDirtyFrame();
  require(gRenders.load() == 1, "one initial frame");
  require(gFooterSets.load() == 2, "cache fill and submit set the footer");
  require(gSubmitGuards.load() == 1, "one submit guard");
  require(gRemember.load() == 0, "cache hit does not remember");
  require(gCopiesAtRender.load() == 2 && gCtorsAtRender.load() == 0, "initial snapshot copy");

  activity->selectedIndex_ = 3;
  gBlockAssign.store(true, std::memory_order_release);
  gLoadGeneration.store(1, std::memory_order_release);
  const int ensureBefore = gEnsure.load();
  activity->verifyDrawerCache_.store(true, std::memory_order_release);
  waitFor(gAssignHolding, "pending move holds both locks");
  require(gLocksHeld.load() == 2, "reload and rendering mutexes held during publish");
  require(!activity->pendingReady_.load(), "ready is stored after both moves");
  require(activity->items_.size() == 4, "UI items stay put while the worker moves pending");
  require(activity->items_[3].id == "item-3", "UI item id stays put");
  require(activity->apps_.size() == 2, "UI apps stay put while the worker moves pending");
  require(activity->apps_[0].id == "plugin-0", "UI app id stays put");
  require(activity->selectedIndex_ == 3, "selection stays put");
  require(gRenders.load() == 1, "no render during the pending move");
  require(gFooterSets.load() == 2, "no footer write during the pending move");
  require(gSubmitGuards.load() == 1, "no submit during the pending move");

  gBlockAssign.store(false, std::memory_order_release);
  waitFor(activity->pendingReady_, "pending published");
  require(activity->items_.size() == 4, "UI items still old after publish");
  require(activity->apps_.size() == 2, "UI apps still old after publish");
  require(activity->apps_[0].id == "plugin-0", "UI app id still old after publish");
  require(activity->selectedIndex_ == 3, "selection still old after publish");
  const int ensure = gEnsure.load();
  require(ensure == ensureBefore + 1, "one scan produced the pending list");
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  require(gEnsure.load() == ensure, "unconsumed pending does not rescan");

  activity->mode_ = 1;
  activity->acceptPendingDrawer();
  require(activity->pendingReady_.load(), "dialog does not apply pending");
  require(activity->items_.size() == 4, "dialog leaves the UI items");
  require(activity->apps_[0].id == "plugin-0", "dialog leaves the UI apps");

  activity->mode_ = 0;
  activity->childScreenOwned_.store(true, std::memory_order_release);
  activity->acceptPendingDrawer();
  require(activity->pendingReady_.load(), "child does not apply pending");
  require(activity->items_.size() == 4, "child leaves the UI items");

  activity->childScreenOwned_.store(false, std::memory_order_release);
  activity->subActivity.reset(new int(1));
  activity->acceptPendingDrawer();
  require(activity->pendingReady_.load(), "subactivity does not apply pending");
  require(activity->items_.size() == 4, "subactivity leaves the UI items");
  activity->subActivity.reset();

  activity->selectedIndex_ = 100;
  const int copies = gCopyAssigns.load();
  const int ctors = gCopyCtors.load();
  activity->acceptPendingDrawer();
  require(gCopyAssigns.load() == copies, "accept does not copy-assign a vector");
  require(gCopyCtors.load() == ctors, "accept does not copy-construct a vector");
  require(activity->apps_.size() == 1, "accepted the new app list");
  require(activity->apps_[0].id == "changed", "accepted app id");
  require(!activity->pendingReady_.load(), "accept consumed pending");
  require(activity->pendingApps_.empty(), "pending apps cleared");
  require(activity->pendingItems_.empty(), "pending items cleared");
  require(activity->selectedIndex_ == static_cast<int>(activity->items_.size()) - 1, "selection clamped");
  require(activity->selectedIndex_ >= 0, "clamped selection is non-negative");
  require(activity->selectedIndex_ < static_cast<int>(activity->items_.size()), "clamped selection in range");
  require(gRemember.load() == 1, "accept remembered a still-valid drawer");

  const int renders = gRenders.load();
  const int assignBefore = gCopyAssigns.load();
  const int ctorBefore = gCopyCtors.load();
  activity->submitDirtyFrame();
  require(gRenders.load() == renders + 1, "post-accept frame rendered once");
  require(gCopyAssigns.load() == assignBefore + 2, "snapshot copy-assigns items and apps once");
  require(gCopyCtors.load() == ctorBefore, "snapshot does not copy-construct a vector");
  require(activity->snapshot_.apps.size() == activity->apps_.size(), "snapshot apps match the UI list");
  require(activity->snapshot_.items.size() == activity->items_.size(), "snapshot items match the UI list");
  require(activity->snapshot_.selectedIndex == activity->selectedIndex_, "snapshot selection matches");
  require(activity->snapshot_.apps[0].id == "changed", "snapshot app id");
  activity->submitDirtyFrame();
  require(gRenders.load() == renders + 1, "clean frame did not render again");
  std::printf(
      "pending-handoff: old_items=4 old_apps=2 accepted_items=%zu selected=%d copy_delta=%d remember=%d ensures=%d\n",
      activity->items_.size(), activity->selectedIndex_, gCopyAssigns.load() - assignBefore, gRemember.load(),
      ensure);
  activity->onExit();
  finishNaturally(activity);
}

static void testPendingDiscardOnExit() {
  resetCounters();
  gCacheHit.store(true);
  auto* activity = new AppListActivity;
  activity->onEnter();
  activity->verifyDrawerCache_.store(false, std::memory_order_release);
  activity->submitDirtyFrame();
  require(gRemember.load() == 0, "cache hit did not remember");
  require(gRenders.load() == 1, "one frame before discard");
  require(gFooterSets.load() == 2, "footer from cache fill and submit");
  require(gSubmitGuards.load() == 1, "one guard before discard");

  gLoadGeneration.store(1, std::memory_order_release);
  const int ensure = gEnsure.load();
  activity->verifyDrawerCache_.store(true, std::memory_order_release);
  waitFor(activity->pendingReady_, "pending before exit");
  require(activity->items_.size() == 4, "UI items unchanged before discard");
  require(activity->apps_.size() == 2, "UI apps unchanged before discard");
  require(activity->apps_[0].id == "plugin-0", "UI app unchanged before discard");
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  require(gEnsure.load() == ensure + 1, "unconsumed pending scanned once");

  const int renders = gRenders.load();
  const int footer = gFooterSets.load();
  const int guards = gSubmitGuards.load();
  const long elapsed = onExitMs(activity);
  require(elapsed < 200, "onExit did not wait to discard");
  simulateHomeDraw();
  const auto start = std::chrono::steady_clock::now();
  while (!activity->displayTaskExited_.load(std::memory_order_acquire)) {
    if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
      std::fprintf(stderr, "timeout waiting for discard publish\n");
      std::abort();
    }
    std::this_thread::yield();
  }
  require(!activity->pendingReady_.load(), "exit cleared pending");
  require(activity->pendingApps_.empty(), "exit destroyed pending apps");
  require(activity->pendingItems_.empty(), "exit destroyed pending items");
  require(gRenders.load() == renders, "exit did not render");
  require(gFooterSets.load() == footer, "exit did not write the footer");
  require(gSubmitGuards.load() == guards, "exit did not submit");
  require(gRemember.load() == 0, "discarded pending did not remember");
  require(gHomeDraws.load() == 1, "home draw ran while the drawer was exiting");
  std::printf("pending-discard: on_exit_ms=%ld renders=%d footer=%d guards=%d remember=%d home_draws=%d\n", elapsed,
              gRenders.load(), gFooterSets.load(), gSubmitGuards.load(), gRemember.load(), gHomeDraws.load());
  finishNaturally(activity);
  std::printf("pending-discard-release: sem_deletes=%d sem_live=%d self_deletes=%d\n", gSemDeletes.load(),
              gSemLive.load(), gSelfDeletes.load());
}

static void testChildSkipsParentPaint() {
  resetCounters();
  gCacheHit.store(true);
  auto* activity = new AppListActivity;
  activity->onEnter();
  activity->verifyDrawerCache_.store(false, std::memory_order_release);
  require(activity->updateRequired_.load(), "enter armed a frame");
  activity->childScreenOwned_.store(true, std::memory_order_release);
  activity->submitDirtyFrame();
  require(gRenders.load() == 0, "child-owned parent did not draw");
  require(activity->updateRequired_.load(), "skipped child frame stayed dirty");
  activity->childScreenOwned_.store(false, std::memory_order_release);
  activity->subActivity.reset(new int(1));
  activity->submitDirtyFrame();
  require(gRenders.load() == 0, "parent did not draw while the child page is up");
  require(activity->updateRequired_.load(), "skipped subactivity frame stayed dirty");
  activity->subActivity.reset();
  activity->submitDirtyFrame();
  require(gRenders.load() == 1, "parent drew once after the child was gone");
  require(!activity->updateRequired_.load(), "the post-child frame consumed the dirty bit");
  std::printf("child-skip: during_child=0 after_child=%d\n", gRenders.load());
  activity->onExit();
  finishNaturally(activity);
}

int main() {
  gM4RenderMutex = new HostSem;
  testDisplayHold();
  testReloadHold();
  testNeverEnter();
  testMutexFail(3, 0, "mutex-both-fail");
  testMutexFail(1, 1, "mutex-first-fail");
  testMutexFail(2, 1, "mutex-second-fail");
  testTaskFail();
  testPublishUaf();
  testDirtyFrame();
  testFrames();
  testPendingHandoff();
  testPendingDiscardOnExit();
  testChildSkipsParentPaint();
  delete gM4RenderMutex;
  gM4RenderMutex = nullptr;
  std::printf("actual app list lifecycle: PASS\n");
  return 0;
}
'''
    code = code.replace('__READY__', ready).replace('__BODIES__', bodies)
    run(code)
    print('actual app list lifecycle: PASS')


if __name__ == '__main__':
    main()
