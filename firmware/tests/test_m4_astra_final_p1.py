"""Astra 2026-09-28 P1 regressions: journal NoMemory, same-version reinstall, retain release.

Compiles the real journal bodies and decideRecovery via recoverAll.
"""
from pathlib import Path
import subprocess
import tempfile

from test_m4_astra_stability import SRC, function

ROOT = Path(__file__).resolve().parents[2]
JSON_CANDIDATES = [
    ROOT / 'firmware/.pio/libdeps/murphy_m4/ArduinoJson',
    Path('/private/tmp/m4-registry-race-20260927/firmware/.pio/libdeps/murphy_m4/ArduinoJson'),
    Path('/private/tmp/m4-hardening-all-20260927/firmware/.pio/libdeps/murphy_m4/ArduinoJson'),
]


def json_inc():
    for p in JSON_CANDIDATES:
        if (p / 'src' / 'ArduinoJson.h').exists() or (p / 'ArduinoJson.h').exists():
            return p
    raise SystemExit('ArduinoJson headers not found')


def test_source_contract():
    inst = (SRC / 'apps/M4xInstaller.cpp').read_text()
    install = function(inst, 'M4xInstallResult M4xInstaller::install(')
    uninstall = function(inst, 'bool M4xInstaller::uninstall(')
    release = function(inst, 'bool M4xInstaller::archiveAndReleasePending(')
    assert 'archiveAndRelease' not in install
    assert 'archiveAndRelease' not in uninstall
    assert release.index('gate.acquire') < release.index('M4xInstallJournal::archiveAndRelease')
    txn = (SRC / 'apps/M4xInstallTxn.h').read_text()
    decision = function(txn, 'inline RecoveryAction decideRecovery(')
    assert 'sameIdentity' in decision
    assert 'liveExists' in decision
    journal = (SRC / 'apps/M4xInstallJournal.cpp').read_text()
    classify = function(journal, 'JournalParse classifyJournalBody(')
    assert 'NoMemory' in classify
    select = function(journal, 'ReconciledLoad selectJournalSnapshot(')
    assert 'SdMan.remove' not in select
    promote = function(journal, 'bool promoteSelectedSnapshot(')
    assert 'v.bakValid' not in promote
    print('astra final p1 source contract: PASS')


def parts():
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
        'bool durableWriteJournal(',
        'ReconciledLoad selectJournalSnapshot(',
        'bool promoteSelectedSnapshot(',
        'bool tryLoadAll(',
        'bool saveAll(',
        'bool upsert(',
        'bool remove(',
        'bool readPending(',
        'int recoverAll(',
        'bool listPendingJournalIds(',
        'bool archiveAndRelease(',
    ]
    return '\n'.join(function(journal, n) for n in names)


def run_cpp():
    inc = json_inc()
    code = r'''
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "apps/M4xPaths.h"
#include "apps/M4xInstallTxn.h"
#include "apps/M4xInstallJournal.h"
#include <ArduinoJson.h>
struct Node { bool dir=false; std::string data; };
static std::map<std::string, Node> disk;
static bool failWrite=false, failRename=false;
static int oomCalls=0, oomAt=-1;
static DeserializationError realParse(JsonDocument& doc, const std::string& raw) {
  return deserializeJson(doc, raw);
}
static DeserializationError hookParse(JsonDocument& doc, const std::string& raw) {
  ++oomCalls;
  if (oomAt > 0 && oomCalls == oomAt) return DeserializationError::NoMemory;
  return realParse(doc, raw);
}
#define deserializeJson hookParse
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
    if (!valid || failWrite) return -1;
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
    if (!disk.count(p) || disk[p].dir) return false;
    f = FsFile(p);
    return true;
  }
  bool openFileForWrite(const char*, const char* p, FsFile& f) {
    if (disk.count(p) || (disk.count(p) && disk[p].dir)) return false;
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
    if (failRename || !disk.count(a) || disk[a].dir || disk.count(b)) return false;
    disk[b]=disk[a]; disk.erase(a); return true;
  }
} SdMan;
namespace M4xInstallJournal {
''' + parts() + r'''
}
static const char* kApp = "com.example.clock";
static int regCode = 1;
static std::string regEntry = "main.lua";
static bool regHas = true;
static int drops = 0, restores = 0, commits = 0;
static bool hookMatch(const M4xInstallTxn::JournalRecord& rec, void*) {
  return regHas && regCode == rec.newVersionCode && regEntry == rec.newEntry;
}
static bool existsPath(const std::string& p, void*) { return disk.count(p) > 0; }
static bool dropBak(const M4xInstallTxn::JournalRecord& rec, void*) {
  ++drops;
  disk.erase(rec.backupPath);
  disk.erase(rec.backupPath + "/main.lua");
  return true;
}
static bool dropStaging(const M4xInstallTxn::JournalRecord& rec, void*) {
  disk.erase(rec.stagingPath);
  disk.erase(rec.stagingPath + "/main.lua");
  return true;
}
static bool restoreOld(const M4xInstallTxn::JournalRecord& rec, void*) {
  ++restores;
  auto bak = disk[rec.backupPath + "/main.lua"].data;
  disk[rec.installPath] = {true, {}};
  disk[rec.installPath + "/main.lua"] = {false, bak};
  disk.erase(rec.backupPath + "/main.lua");
  disk.erase(rec.backupPath);
  return disk[rec.installPath + "/main.lua"].data == "OLD";
}
static bool commitNew(const M4xInstallTxn::JournalRecord& rec, void*) {
  ++commits;
  regCode = rec.newVersionCode;
  regEntry = rec.newEntry;
  regHas = true;
  return true;
}
static std::string journal(const char* phase, int oldCode, int newCode) {
  return std::string("{\"txns\":[{\"id\":\"") + kApp +
    "\",\"phase\":\"" + phase + "\",\"hadPriorInstall\":true,"
    "\"installPath\":\"/apps/com.example.clock\","
    "\"stagingPath\":\"/apps/com.example.clock.staging\","
    "\"backupPath\":\"/apps/com.example.clock.bak\","
    "\"oldVersionCode\":" + std::to_string(oldCode) + ",\"newVersionCode\":" + std::to_string(newCode) + ","
    "\"oldEntry\":\"main.lua\",\"newEntry\":\"main.lua\"}]}";
}
static void plant(const std::string& body) {
  disk.clear();
  disk[M4xInstallTxn::kJournalPath] = {false, body};
  disk["/apps/com.example.clock.bak"] = {true, {}};
  disk["/apps/com.example.clock.bak/main.lua"] = {false, "OLD"};
  disk["/apps/com.example.clock.staging"] = {true, {}};
  disk["/apps/com.example.clock.staging/main.lua"] = {false, "NEW"};
  drops = restores = commits = 0;
  oomCalls = 0; oomAt = -1;
  regHas = true; regCode = 1; regEntry = "main.lua";
}
static M4xInstallJournal::RecoveryHooks hooks() {
  M4xInstallJournal::RecoveryHooks h;
  h.liveExists = existsPath;
  h.bakExists = existsPath;
  h.stagingExists = existsPath;
  h.registryMatchesNew = hookMatch;
  h.dropBak = dropBak;
  h.dropStaging = dropStaging;
  h.restoreOldFromBak = restoreOld;
  h.commitNewRegistry = commitNew;
  return h;
}
static bool pathExists(const std::string& p, void*) { return disk.count(p) > 0; }
static bool listTree(const std::string& root, std::vector<std::string>& rels, void*) {
  rels.clear();
  const std::string prefix = root.back()=='/' ? root : root + "/";
  for (const auto& it : disk) {
    if (!it.second.dir && it.first.compare(0, prefix.size(), prefix) == 0)
      rels.push_back(it.first.substr(prefix.size()));
  }
  return true;
}
static bool copyFile(const std::string& src, const std::string& dst, void*) {
  if (!disk.count(src) || disk[src].dir || disk.count(dst)) return false;
  for (size_t i = 1; i < dst.size(); ++i) if (dst[i]=='/') disk[dst.substr(0,i)] = {true,{}};
  disk[dst] = disk[src];
  std::string back;
  return disk[dst].data == disk[src].data;
}
int main() {
  const std::string primary = journal("quarantined", 1, 1);
  const std::string bak = journal("staging", 1, 1);
  plant(primary);
  disk[std::string(M4xInstallTxn::kJournalPath) + ".bak"] = {false, bak};
  const auto primaryBytes = disk[M4xInstallTxn::kJournalPath].data;
  const auto bakBytes = disk[std::string(M4xInstallTxn::kJournalPath) + ".bak"].data;
  oomAt = 1;
  assert(M4xInstallJournal::recoverAll(hooks()) == 0);
  assert(drops == 0 && restores == 0 && commits == 0);
  assert(disk[M4xInstallTxn::kJournalPath].data == primaryBytes);
  assert(disk[std::string(M4xInstallTxn::kJournalPath) + ".bak"].data == bakBytes);
  assert(!disk.count(std::string(M4xInstallTxn::kJournalPath) + ".tmp"));
  oomAt = oomCalls + 1;
  M4xInstallTxn::JournalRecord ignored;
  assert(!M4xInstallJournal::upsert(ignored));
  oomAt = oomCalls + 1;
  assert(!M4xInstallJournal::remove(kApp));
  assert(disk[M4xInstallTxn::kJournalPath].data == primaryBytes);

  plant("{");
  disk[std::string(M4xInstallTxn::kJournalPath) + ".bak"] = {false, bak};
  disk[std::string(M4xInstallTxn::kJournalPath) + ".tmp"] = {false, journal("live_switched", 1, 2)};
  failWrite = true; failRename = true;
  assert(M4xInstallJournal::recoverAll(hooks()) == 0);
  assert(disk.count(std::string(M4xInstallTxn::kJournalPath) + ".tmp"));
  assert(disk[std::string(M4xInstallTxn::kJournalPath) + ".bak"].data == bak);
  failWrite = false; failRename = false;

  plant(journal("quarantined", 1, 1));
  regCode = 1;
  assert(M4xInstallJournal::recoverAll(hooks()) == 1);
  assert(restores == 1);
  assert(drops == 0);
  assert(disk["/apps/com.example.clock/main.lua"].data == "OLD");
  assert(disk.count("/apps/com.example.clock.bak/main.lua") == 0);
  assert(!(disk.count("/apps/com.example.clock.bak/main.lua") && disk.count("/apps/com.example.clock.staging/main.lua") == 0 &&
           disk.count("/apps/com.example.clock/main.lua") == 0));

  plant(journal("live_switched", 1, 1));
  disk["/apps/com.example.clock"] = {true, {}};
  disk["/apps/com.example.clock/main.lua"] = {false, "OLD"};
  regCode = 1;
  assert(M4xInstallJournal::recoverAll(hooks()) == 1);
  assert(disk["/apps/com.example.clock/main.lua"].data == "OLD");
  assert(regCode == 1);

  plant(journal("registry_committed", 1, 2));
  regCode = 2;
  assert(M4xInstallJournal::recoverAll(hooks()) == 1);
  assert(drops == 0);
  assert(disk.count("/apps/com.example.clock.bak/main.lua"));

  plant(journal("registry_committed", 1, 2));
  disk["/apps/com.example.clock"] = {true, {}};
  disk["/apps/com.example.clock/main.lua"] = {false, "NEW"};
  regCode = 2;
  assert(M4xInstallJournal::recoverAll(hooks()) == 1);
  assert(drops == 1);
  assert(!disk.count("/apps/com.example.clock.bak/main.lua"));
  assert(disk["/apps/com.example.clock/main.lua"].data == "NEW");

  plant(journal("live_switched", 1, 2));
  disk.erase("/apps/com.example.clock.bak");
  disk.erase("/apps/com.example.clock.bak/main.lua");
  disk["/apps/com.example.clock"] = {true, {}};
  disk["/apps/com.example.clock/main.lua"] = {false, "OLD"};
  regCode = 1;
  assert(M4xInstallJournal::recoverAll(hooks()) == 1);
  assert(commits == 0 && drops == 0);
  assert(disk[M4xInstallTxn::kJournalPath].data.find("live_switched") != std::string::npos);
  assert(disk["/apps/com.example.clock/main.lua"].data == "OLD");

  std::vector<M4xInstallJournal::PendingJournalId> ids;
  assert(M4xInstallJournal::listPendingJournalIds(ids, [](const std::string&, void*) { return false; }, nullptr));
  assert(ids.size() == 1 && ids[0].id == kApp && !ids[0].inRegistry);

  M4xInstallJournal::ArchiveHooks ah;
  ah.pathExists = pathExists;
  ah.listTree = listTree;
  ah.copyFile = [](const std::string&, const std::string&, void*) { return false; };
  std::string err;
  const auto before = disk[M4xInstallTxn::kJournalPath].data;
  assert(!M4xInstallJournal::archiveAndRelease(kApp, ah, err));
  assert(err == "archive_copy");
  assert(disk[M4xInstallTxn::kJournalPath].data == before);
  assert(disk.count("/system/m4x_archive/com.example.clock"));
  const auto partialSnap = disk["/system/m4x_archive/com.example.clock/snapshot.json"].data;
  assert(partialSnap == before);
  disk["/apps_data/com.example.clock/note"] = {false, "KEEP"};
  ah.copyFile = copyFile;
  assert(M4xInstallJournal::archiveAndRelease(kApp, ah, err));
  assert(disk["/system/m4x_archive/com.example.clock/snapshot.json"].data == partialSnap);
  assert(disk["/system/m4x_archive/com.example.clock-2/snapshot.json"].data == before);
  assert(disk["/system/m4x_archive/com.example.clock-2/live/main.lua"].data == "OLD");
  assert(disk["/apps/com.example.clock/main.lua"].data == "OLD");
  assert(disk["/apps_data/com.example.clock/note"].data == "KEEP");
  bool pendingRetry = true;
  assert(M4xInstallJournal::readPending(kApp, pendingRetry) && !pendingRetry);

  plant(journal("live_switched", 1, 2));
  disk.erase("/apps/com.example.clock.bak");
  disk.erase("/apps/com.example.clock.bak/main.lua");
  disk["/apps/com.example.clock"] = {true, {}};
  disk["/apps/com.example.clock/main.lua"] = {false, "OLD"};
  const auto beforeOk = disk[M4xInstallTxn::kJournalPath].data;
  ah.copyFile = copyFile;
  assert(M4xInstallJournal::archiveAndRelease(kApp, ah, err));
  assert(disk["/system/m4x_archive/com.example.clock/snapshot.json"].data == beforeOk);
  assert(disk["/system/m4x_archive/com.example.clock/live/main.lua"].data == "OLD");
  bool pending = true;
  assert(M4xInstallJournal::readPending(kApp, pending) && !pending);
  assert(disk.count("/apps_data/com.example.clock") == 0);
  return 0;
}
'''
    inc_flag = f'-I{inc}/src' if (inc / 'src' / 'ArduinoJson.h').exists() else f'-I{inc}'
    with tempfile.TemporaryDirectory(prefix='m4-astra-p1-') as tmp:
        cpp = Path(tmp) / 'test.cpp'
        exe = Path(tmp) / 'test'
        cpp.write_text(code)
        subprocess.run(['c++', '-std=c++17', '-O1', '-g', inc_flag, f'-I{ROOT}/firmware/src', str(cpp), '-o', str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True)
    print('astra final p1 host: PASS')


if __name__ == '__main__':
    test_source_contract()
    run_cpp()
