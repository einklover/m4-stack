"""Same-id retry must not replace a LiveSwitched journal.

Extracts real readPending, refuseIfPendingJournal, and recoverAll.
Does not compile M4xInstaller::install, ZipFile, production removeTree, or SdFat.
A host map stands in for the card. Registry commit is a test hook.
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


def test_source_order():
    inst = (SRC / 'apps/M4xInstaller.cpp').read_text()
    install = function(inst, 'M4xInstallResult M4xInstaller::install(')
    uninstall = function(inst, 'bool M4xInstaller::uninstall(')
    guard = function(inst, 'bool refuseIfPendingJournal(')
    assert install.index('probe(') < install.index('refuseIfPendingJournal')
    assert install.index('refuseIfPendingJournal') < install.index('removeTreeBestEffort(staging')
    assert install.index('refuseIfPendingJournal') < install.index('M4xInstallJournal::upsert')
    assert 'M4xInstallJournal::remove' not in install[:install.index('refuseIfPendingJournal')]
    assert uninstall.index('gate.acquire') < uninstall.index('refuseIfPendingJournal')
    assert uninstall.index('refuseIfPendingJournal') < uninstall.index('M4xRegistry::remove')
    assert 'removeTreeBestEffort' not in uninstall[:uninstall.index('refuseIfPendingJournal')]
    assert guard.index('readPending') < guard.index('recovery_required')
    assert 'upsert' not in guard and 'remove(' not in guard
    journal = (SRC / 'apps/M4xInstallJournal.cpp').read_text()
    pending = function(journal, 'bool readPending(')
    assert 'saveAll' not in pending and 'durableWriteJournal' not in pending
    assert 'SdMan.remove' not in pending
    assert 'decideLoad' in pending
    assert 'parseBody(' not in pending
    assert 'deserializeJson(doc, *snap)' in pending
    assert 'doc.overflowed()' in pending
    assert 'return false' in pending
    assert 'find(' not in guard
    print('pending retry source order: PASS')


def parts():
    journal = (SRC / 'apps/M4xInstallJournal.cpp').read_text()
    inst = (SRC / 'apps/M4xInstaller.cpp').read_text()
    names = [
        'bool readExactFile(',
        'bool writeExactFile(',
        'bool copyFileExact(',
        'bool renameOrCopy(',
        'bool isValidJournalBody(',
        'void pushStringArray(',
        'void readStringArray(',
        'M4xInstallTxn::JournalRecord parseOne(',
        'void writeOne(',
        'std::vector<M4xInstallTxn::JournalRecord> parseBody(',
        'bool durableWriteJournal(',
        'struct ReconciledLoad {',
        'ReconciledLoad loadReconciledRawStatus(',
        'std::string loadReconciledRaw(',
        'bool tryLoadAll(',
        'std::vector<M4xInstallTxn::JournalRecord> loadAll(',
        'bool saveAll(',
        'bool upsert(',
        'bool remove(',
        'M4xInstallTxn::JournalRecord find(',
        'bool readPending(',
        'int recoverAll(',
    ]
    return '\n'.join((function(journal, n)+';' if n.startswith('struct ReconciledLoad') else function(journal, n)) for n in names), function(inst, 'bool refuseIfPendingJournal(')


def run_cpp():
    inc = json_inc()
    journal_body, guard = parts()
    code = r'''
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include "apps/M4xPaths.h"
#include "apps/M4xInstallTxn.h"
#include "apps/M4xInstallJournal.h"
#include <ArduinoJson.h>
struct Node { bool dir=false; std::string data; };
static std::map<std::string, Node> disk;
static bool failRead = false;
struct SerialShim { template<class... A> void printf(const char*, A...) {} } Serial;
struct FsFile {
  std::string path; bool valid=false; size_t at=0;
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
    return (int)nread;
  }
  int write(const uint8_t* p, size_t n) {
    if (!valid) return -1;
    disk[path].data.append(reinterpret_cast<const char*>(p), n);
    return (int)n;
  }
  bool getWriteError() const { return false; }
  bool sync() { return true; }
  bool close() { valid=false; return true; }
};
struct SdShim {
  bool exists(const char* p) const { return disk.count(p); }
  bool mkdir(const char* p, bool) { disk[p]=Node{true,{}}; return true; }
  bool openFileForRead(const char*, const char* p, FsFile& f) {
    if (failRead) return false;
    if (!disk.count(p) || disk[p].dir) return false;
    f = FsFile(p);
    return true;
  }
  bool openFileForWrite(const char*, const char* p, FsFile& f) {
    if (disk.count(p) && disk[p].dir) return false;
    disk[p] = Node{false,{}};
    f = FsFile(p);
    return true;
  }
  bool remove(const char* p) {
    if (!disk.count(p) || disk[p].dir) return false;
    disk.erase(p);
    return true;
  }
  bool rename(const char* a, const char* b) {
    if (!disk.count(a) || disk[a].dir || disk.count(b)) return false;
    disk[b]=disk[a]; disk.erase(a); return true;
  }
} SdMan;
namespace M4xInstallJournal {
''' + journal_body + r'''
}
''' + guard + r'''
static const char* kApp = "com.example.clock";
static const char* kLive = "/apps/com.example.clock/main.lua";
static const char* kBak = "/apps/com.example.clock.bak/main.lua";
static std::string registry = "v1";
static bool existsPath(const std::string& p, void*) { return disk.count(p) > 0; }
static bool matchNew(const M4xInstallTxn::JournalRecord&, void*) { return registry == "v2"; }
static bool noId(const std::string&, void*) { return registry == "v1" || registry == "v2"; }
static bool commitNew(const M4xInstallTxn::JournalRecord& rec, void*) {
  if (rec.newVersionCode != 2) return false;
  registry = "v2";
  return true;
}
static bool dropBak(const M4xInstallTxn::JournalRecord& rec, void*) {
  disk.erase(kBak);
  disk.erase(rec.backupPath);
  return !disk.count(rec.backupPath);
}
static bool dropStaging(const M4xInstallTxn::JournalRecord& rec, void*) {
  disk.erase(rec.stagingPath);
  return true;
}
static bool hasPhase(const char* phase) {
  const auto raw = disk[M4xInstallTxn::kJournalPath].data;
  return raw.find(kApp) != std::string::npos && raw.find(phase) != std::string::npos;
}
int main() {
  M4xInstallTxn::JournalRecord rec;
  rec.id = kApp;
  rec.phase = M4xInstallTxn::Phase::LiveSwitched;
  rec.hadPriorInstall = true;
  rec.installPath = "/apps/com.example.clock";
  rec.stagingPath = "/apps/com.example.clock.staging";
  rec.backupPath = "/apps/com.example.clock.bak";
  rec.newVersion = "2.0";
  rec.newVersionCode = 2;
  rec.newEntry = "main.lua";
  rec.oldVersionCode = 1;
  rec.oldEntry = "main.lua";
  assert(M4xInstallJournal::upsert(rec));
  disk[rec.installPath] = {true, {}};
  disk[rec.backupPath] = {true, {}};
  disk[kLive] = {false, "LIVE-V2"};
  disk[kBak] = {false, "BAK-V1"};
  const std::string journalBefore = disk[M4xInstallTxn::kJournalPath].data;
  assert(hasPhase("live_switched"));

  bool pending = false;
  assert(M4xInstallJournal::readPending(kApp, pending) && pending);
  std::string err;
  assert(refuseIfPendingJournal(kApp, err) && err == "recovery_required");
  err.clear();
  assert(refuseIfPendingJournal(kApp, err) && err == "recovery_required");
  assert(disk[M4xInstallTxn::kJournalPath].data == journalBefore);
  assert(disk[kLive].data == "LIVE-V2");
  assert(disk[kBak].data == "BAK-V1");

  failRead = true;
  pending = true;
  assert(!M4xInstallJournal::readPending(kApp, pending));
  assert(!pending);
  err.clear();
  assert(refuseIfPendingJournal(kApp, err) && err == "journal_read");
  assert(disk[M4xInstallTxn::kJournalPath].data == journalBefore);
  assert(disk[kLive].data == "LIVE-V2");
  assert(disk[kBak].data == "BAK-V1");
  failRead = false;

  M4xInstallJournal::RecoveryHooks h;
  h.liveExists = &existsPath;
  h.bakExists = &existsPath;
  h.stagingExists = &existsPath;
  h.registryHasId = &noId;
  h.registryMatchesNew = &matchNew;
  h.commitNewRegistry = &commitNew;
  h.dropBak = &dropBak;
  h.dropStaging = &dropStaging;
  assert(M4xInstallJournal::recoverAll(h) == 1);
  assert(registry == "v2");
  assert(disk[kLive].data == "LIVE-V2");
  assert(!disk.count(kBak));
  assert(!hasPhase("live_switched"));
  pending = true;
  assert(M4xInstallJournal::readPending(kApp, pending) && !pending);
  const std::string liveAfter = disk[kLive].data;
  const std::string journalAfter = disk[M4xInstallTxn::kJournalPath].data;
  assert(M4xInstallJournal::recoverAll(h) == 0);
  assert(disk[kLive].data == liveAfter);
  assert(disk[M4xInstallTxn::kJournalPath].data == journalAfter);
  assert(registry == "v2");

  const std::string jp = M4xInstallTxn::kJournalPath;
  const std::string jbak = jp + ".bak";
  const std::string jtmp = jp + ".tmp";
  auto plant = [](const std::string& path, const char* body) {
    disk[path] = Node{false, body};
  };

  disk.erase(jtmp);
  plant(jp, "{\"txns\":[]}");
  plant(jbak, "{\"txns\":[{\"id\":\"com.example.clock\",\"phase\":\"staging\"}]}");
  const std::string primarySnap = disk[jp].data;
  const std::string bakSnap = disk[jbak].data;
  pending = true;
  assert(M4xInstallJournal::readPending(kApp, pending) && !pending);
  err.clear();
  assert(!refuseIfPendingJournal(kApp, err));
  assert(disk[jp].data == primarySnap);
  assert(disk[jbak].data == bakSnap);
  assert(!disk.count(jtmp));

  plant(jp, "NOT-JSON");
  plant(jbak, "{\"txns\":[{\"id\":\"com.example.clock\",\"phase\":\"live_switched\"}]}");
  disk.erase(jtmp);
  pending = false;
  assert(M4xInstallJournal::readPending(kApp, pending) && pending);
  err.clear();
  assert(refuseIfPendingJournal(kApp, err) && err == "recovery_required");
  assert(disk[jp].data == "NOT-JSON");
  assert(disk[jbak].data.find("live_switched") != std::string::npos);

  disk.erase(jp);
  plant(jbak, "{\"txns\":[{\"id\":\"com.example.other\",\"phase\":\"staging\"}]}");
  plant(jtmp, "{\"txns\":[{\"id\":\"com.example.clock\",\"phase\":\"staging\"}]}");
  pending = false;
  assert(M4xInstallJournal::readPending(kApp, pending) && pending);
  pending = true;
  assert(M4xInstallJournal::readPending("com.example.other", pending) && !pending);
  assert(disk[jtmp].data.find(kApp) != std::string::npos);
  assert(disk[jbak].data.find("com.example.other") != std::string::npos);
  assert(!disk.count(jp));
}
'''
    with tempfile.TemporaryDirectory(prefix='m4-pending-') as tmp:
        cpp = Path(tmp) / 'test.cpp'
        exe = Path(tmp) / 'test'
        cpp.write_text(code)
        subprocess.run(
            ['c++', '-std=c++17', '-O1', '-g', '-fsanitize=address,undefined',
             '-fno-omit-frame-pointer',
             '-I', str(SRC), '-I', str(inc), '-I', str(inc / 'src'),
             str(cpp), '-o', str(exe)],
            check=True)
        subprocess.run([str(exe)], check=True)
    print('pending retry interleave: PASS')


if __name__ == '__main__':
    test_source_order()
    run_cpp()
