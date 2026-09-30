"""Install recovery must not run from layout/probe/reload.

Compiles the real journal load/save/recoverAll bodies. A host mutex stands in
for the non-recursive install gate (production: xSemaphoreCreateMutex). This
does not run ZipFile extract, production removeTree, or SdFat. Crash recovery
is the real decideRecovery path inside recoverAll with test hooks.
"""
from pathlib import Path
import subprocess
import tempfile

from test_m4_astra_stability import SRC, function

ROOT = Path(__file__).resolve().parents[2]
JSON_CANDIDATES = [
    ROOT / 'firmware/.pio/libdeps/murphy_m4/ArduinoJson',
    Path('/private/tmp/m4-registry-race-20260927/firmware/.pio/libdeps/murphy_m4/ArduinoJson'),
]


def json_inc():
    for p in JSON_CANDIDATES:
        if (p / 'src' / 'ArduinoJson.h').exists() or (p / 'ArduinoJson.h').exists():
            return p
    raise SystemExit('ArduinoJson headers not found')


def test_source_contract():
    inst = (SRC / 'apps/M4xInstaller.cpp').read_text()
    header = (SRC / 'apps/M4xInstaller.h').read_text()
    ensure = function(inst, 'void M4xInstaller::ensureLayout()')
    recover = function(inst, 'void M4xInstaller::recoverInterrupted()')
    install = function(inst, 'M4xInstallResult M4xInstaller::install(')
    uninstall = function(inst, 'bool M4xInstaller::uninstall(')
    probe = function(inst, 'M4xInstallResult M4xInstaller::probe(')
    locked = function(inst, 'void recoverInterruptedInstallsLocked()')
    assert 'recoverInterrupted' not in ensure
    assert 'installGate' not in ensure and 'gate.acquire' not in ensure
    assert 'SdMan.mkdir' in ensure
    assert recover.index('gate.acquire') < recover.index('recoverInterruptedInstallsLocked')
    assert recover.count('recoverInterruptedInstallsLocked') == 1
    assert 'xSemaphoreTake' not in locked and 'gate.acquire' not in locked
    assert 'recoverAll' in locked
    assert 'recoverInterrupted' not in install
    assert 'recoverInterruptedInstallsLocked' not in install
    assert install.index('gate.acquire') < install.index('probe(')
    assert 'recoverInterrupted' not in uninstall
    assert uninstall.index('gate.acquire') < uninstall.index('M4xIsValidPackageId')
    assert probe.index('ensureLayout();') < probe.index('not_found')
    assert 'recoverInterrupted' not in probe
    assert inst.count('void recoverInterruptedInstallsLocked()') == 1
    assert inst.count('recoverInterruptedInstallsLocked()') == 2  # def + one call
    assert 'static void recoverInterrupted();' in header
    assert 'xSemaphoreCreateMutex' in inst
    assert 'recursive' not in inst.split('installGateHandle')[1][:400]

    reload = function((SRC / 'activities/apps/AppListActivity.cpp').read_text(),
                      'bool AppListActivity::reload(')
    assert 'ensureLayout' in reload and 'recoverInterrupted' not in reload
    http = (SRC / 'network/M4FileTransferHttpRoutes.cpp').read_text()
    assert 'M4xInstaller::ensureLayout' in http
    assert 'recoverInterrupted' not in http
    assert http.index('M4xInstaller::ensureLayout') < http.index('M4xInstaller::install')

    main = (SRC / 'main.cpp').read_text()
    assert main.count('M4xInstaller::recoverInterrupted()') == 1
    assert main.index('Home scene bridge ready') < main.index('M4xInstaller::recoverInterrupted()')
    assert main.index('[M4-SD] mounted ok') < main.index('M4xInstaller::recoverInterrupted()')
    assert main.index('M4xInstaller::recoverInterrupted()') < main.index('SETTINGS.loadFromFile()')
    print('install recovery source contract: PASS')


def journal_parts():
    journal = (SRC / 'apps/M4xInstallJournal.cpp').read_text()
    names = [
        'bool readExactFile(',
        'bool writeExactFile(',
        'bool copyFileExact(',
        'bool renameOrCopy(',
        'JournalParse classifyJournalBody(',
        'bool isValidJournalBody(',
        'void pushStringArray(',
        'void readStringArray(',
        'M4xInstallTxn::JournalRecord parseOne(',
        'void writeOne(',
        'std::vector<M4xInstallTxn::JournalRecord> parseBody(',
        'bool durableWriteJournal(',
        'ReconciledLoad selectJournalSnapshot(',
        'bool promoteSelectedSnapshot(',
        'std::string loadReconciledRaw(',
        'bool tryLoadAll(',
        'std::vector<M4xInstallTxn::JournalRecord> loadAll(',
        'bool saveAll(',
        'bool upsert(',
        'bool remove(',
        'M4xInstallTxn::JournalRecord find(',
        'int recoverAll(',
    ]
    return '\n'.join(function(journal, n) for n in names)


def run_cpp():
    inc = json_inc()
    code = r'''
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include "apps/M4xPaths.h"
#include "apps/M4xInstallTxn.h"
#include "apps/M4xInstallJournal.h"
#include <ArduinoJson.h>
struct Node { bool dir=false; std::string data; };
static std::map<std::string, Node> disk;
static std::mutex diskMu;
static std::mutex gate;
static std::mutex pauseMu;
static std::condition_variable pauseCv;
static bool blockJournalRead = false;
static bool loadEntered = false;
static bool releaseLoad = false;
static int journalOpens = 0;
struct SerialShim { template<class... A> void printf(const char*, A...) {} } Serial;
struct FsFile {
  std::string path; bool valid=false; size_t at=0; bool writeErr=false;
  FsFile()=default;
  explicit FsFile(std::string p): path(std::move(p)), valid(disk.count(path)&&!disk[path].dir) {}
  explicit operator bool() const { return valid; }
  uint64_t fileSize() const { return valid ? disk.at(path).data.size() : 0; }
  int read(uint8_t* p, size_t n) {
    if (!valid) return -1;
    auto& data = disk.at(path).data;
    if (at >= data.size()) return 0;
    size_t nread = std::min(n, data.size()-at);
    memcpy(p, data.data()+at, nread);
    at += nread;
    if (blockJournalRead && path==M4xInstallTxn::kJournalPath && journalOpens==0) {
      journalOpens++;
      std::unique_lock<std::mutex> lk(pauseMu);
      loadEntered = true;
      pauseCv.notify_all();
      pauseCv.wait(lk, []{ return releaseLoad; });
    }
    return (int)nread;
  }
  int write(const uint8_t* p, size_t n) {
    if (!valid) return -1;
    disk[path].data.append(reinterpret_cast<const char*>(p), n);
    return (int)n;
  }
  bool getWriteError() const { return writeErr; }
  bool sync() { return true; }
  bool close() { valid=false; return true; }
};
struct SdShim {
  bool exists(const char* p) const { return disk.count(p); }
  bool mkdir(const char* p, bool) { disk[p]=Node{true,{}}; return true; }
  bool openFileForRead(const char*, const char* p, FsFile& f) {
    std::lock_guard<std::mutex> dk(diskMu);
    if (!disk.count(p) || disk[p].dir) return false;
    f = FsFile(p);
    return true;
  }
  bool openFileForWrite(const char*, const char* p, FsFile& f) {
    std::lock_guard<std::mutex> dk(diskMu);
    if (disk.count(p) && disk[p].dir) return false;
    disk[p] = Node{false,{}};
    f = FsFile(p);
    return true;
  }
  bool remove(const char* p) {
    std::lock_guard<std::mutex> dk(diskMu);
    if (!disk.count(p) || disk[p].dir) return false;
    disk.erase(p);
    return true;
  }
  bool rename(const char* a, const char* b) {
    std::lock_guard<std::mutex> dk(diskMu);
    if (!disk.count(a) || disk[a].dir || disk.count(b)) return false;
    disk[b]=disk[a]; disk.erase(a); return true;
  }
} SdMan;
namespace M4xInstallJournal {
''' + journal_parts() + r'''
}
static M4xInstallTxn::JournalRecord stagingRec(const char* id) {
  M4xInstallTxn::JournalRecord r;
  r.id = id;
  r.phase = M4xInstallTxn::Phase::Staging;
  r.installPath = std::string("/apps/") + id;
  r.stagingPath = r.installPath + ".staging";
  r.backupPath = r.installPath + ".bak";
  r.newVersionCode = 1;
  r.newEntry = "main.lua";
  return r;
}
static bool dropStaging(const M4xInstallTxn::JournalRecord& rec, void*) {
  std::lock_guard<std::mutex> dk(diskMu);
  disk.erase(rec.stagingPath);
  disk.erase(rec.stagingPath + "/main.lua");
  return true;
}
static bool existsPath(const std::string& p, void*) {
  std::lock_guard<std::mutex> dk(diskMu);
  return disk.count(p) > 0;
}
static bool no(const M4xInstallTxn::JournalRecord&, void*) { return false; }
static bool noId(const std::string&, void*) { return false; }
static M4xInstallJournal::RecoveryHooks hooks() {
  M4xInstallJournal::RecoveryHooks h;
  h.liveExists = &existsPath;
  h.bakExists = &existsPath;
  h.stagingExists = &existsPath;
  h.registryHasId = &noId;
  h.registryMatchesNew = &no;
  h.dropStaging = &dropStaging;
  return h;
}
static bool hasId(const char* id) {
  for (const auto& r : M4xInstallJournal::loadAll()) if (r.id==id) return true;
  return false;
}
static void armBlock() {
  blockJournalRead = true;
  loadEntered = false;
  releaseLoad = false;
  journalOpens = 0;
}
int main() {
  const char* journal = M4xInstallTxn::kJournalPath;
  // Crash leftover, no concurrent install: real recoverAll drops staging.
  {
    disk.clear();
    disk[journal] = {false, "{\"txns\":[{\"id\":\"oldcrash\",\"phase\":\"staging\",\"installPath\":\"/apps/oldcrash\",\"stagingPath\":\"/apps/oldcrash.staging\",\"backupPath\":\"/apps/oldcrash.bak\",\"hadPriorInstall\":false,\"newVersionCode\":1,\"newEntry\":\"main.lua\"}]}"};
    disk["/apps/oldcrash.staging/main.lua"] = {false, "OLD"};
    assert(M4xInstallJournal::recoverAll(hooks())==1);
    assert(!disk.count("/apps/oldcrash.staging/main.lua"));
    assert(!hasId("oldcrash"));
  }
  // Public layout/probe path does not call recoverAll: active staging stays.
  {
    disk.clear();
    auto active = stagingRec("active");
    assert(M4xInstallJournal::upsert(active));
    disk[active.stagingPath + "/main.lua"] = {false, "LIVEEXTRACT"};
    disk["/apps"] = {true,{}};
    disk["/apps_data"] = {true,{}};
    disk["/apps_inbox"] = {true,{}};
    disk["/system"] = {true,{}};
    assert(disk[active.stagingPath + "/main.lua"].data=="LIVEEXTRACT");
    assert(hasId("active"));
  }
  // Unlocked recoverAll paused in loadAll clobbers a journal published mid-recovery.
  {
    disk.clear();
    disk[journal] = {false, "{\"txns\":[]}"};
    armBlock();
    std::thread t2([&]{ M4xInstallJournal::recoverAll(hooks()); });
    {
      std::unique_lock<std::mutex> lk(pauseMu);
      pauseCv.wait(lk, []{ return loadEntered; });
    }
    auto active = stagingRec("active");
    assert(M4xInstallJournal::upsert(active));
    disk[active.stagingPath + "/main.lua"] = {false, "LIVEEXTRACT"};
    {
      std::lock_guard<std::mutex> lk(pauseMu);
      releaseLoad = true;
    }
    pauseCv.notify_all();
    t2.join();
    blockJournalRead = false;
    assert(!hasId("active"));
    assert(disk.count(active.stagingPath + "/main.lua"));
  }
  // Same window with the install gate: T1 upsert waits until recoverAll finishes.
  {
    disk.clear();
    disk[journal] = {false, "{\"txns\":[{\"id\":\"oldcrash\",\"phase\":\"staging\",\"installPath\":\"/apps/oldcrash\",\"stagingPath\":\"/apps/oldcrash.staging\",\"backupPath\":\"/apps/oldcrash.bak\",\"hadPriorInstall\":false,\"newVersionCode\":1,\"newEntry\":\"main.lua\"}]}"};
    disk["/apps/oldcrash.staging/main.lua"] = {false, "OLD"};
    armBlock();
    std::atomic<int> order{0};
    int t2done = 0, t1done = 0;
    std::thread t2([&]{
      std::lock_guard<std::mutex> g(gate);
      M4xInstallJournal::recoverAll(hooks());
      t2done = ++order;
    });
    {
      std::unique_lock<std::mutex> lk(pauseMu);
      pauseCv.wait(lk, []{ return loadEntered; });
    }
    std::atomic<bool> t1waiting{false};
    std::atomic<bool> t1entered{false};
    std::thread t1([&]{
      t1waiting = true;
      std::lock_guard<std::mutex> g(gate);
      t1entered = true;
      auto active = stagingRec("active");
      assert(M4xInstallJournal::upsert(active));
      disk[active.stagingPath + "/main.lua"] = {false, "LIVEEXTRACT"};
      t1done = ++order;
    });
    while (!t1waiting.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(!t1entered.load());
    {
      std::lock_guard<std::mutex> lk(pauseMu);
      releaseLoad = true;
    }
    pauseCv.notify_all();
    t2.join();
    t1.join();
    blockJournalRead = false;
    assert(t2done==1 && t1done==2);
    assert(!hasId("oldcrash"));
    assert(!disk.count("/apps/oldcrash.staging/main.lua"));
    assert(hasId("active"));
    assert(disk["/apps/active.staging/main.lua"].data=="LIVEEXTRACT");
  }
}
'''
    # atomic and chrono used
    code = code.replace('#include <condition_variable>',
                        '#include <atomic>\n#include <chrono>\n#include <condition_variable>\n#include <cassert>')
    with tempfile.TemporaryDirectory(prefix='m4-recover-') as tmp:
        cpp = Path(tmp) / 'test.cpp'
        exe = Path(tmp) / 'test'
        cpp.write_text(code)
        subprocess.run(
            ['c++', '-std=c++17', '-O1', '-g', '-fsanitize=address,undefined',
             '-fno-omit-frame-pointer', '-pthread',
             '-I', str(SRC), '-I', str(inc), '-I', str(inc / 'src'),
             str(cpp), '-o', str(exe)],
            check=True)
        subprocess.run([str(exe)], check=True)
    print('install recovery gate interleave: PASS')


if __name__ == '__main__':
    test_source_contract()
    run_cpp()
