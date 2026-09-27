"""Boot journal fail-closed regression (P1-2).

Compiles the real M4xInstallJournal bodies against an in-memory SD shim.
Cases: healthy absent, healthy valid-empty, present-but-unreadable primary
(with valid bak: must NOT promote stale bak), malformed selected snapshot,
valid pending recovery. On load failure: tryLoadAll false, recoverAll == 0,
no hooks run, no journal writes, disk byte-identical.
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
    cpp = (SRC / 'apps/M4xInstallJournal.cpp').read_text()
    header = (SRC / 'apps/M4xInstallJournal.h').read_text()
    assert 'bool tryLoadAll(' in header
    assert 'SdMan.exists(path)' in cpp
    rec = function(cpp, 'int recoverAll(')
    assert 'tryLoadAll' in rec
    assert rec.index('tryLoadAll') < rec.index('remaining')
    assert 'saveAll' not in rec.split('tryLoadAll')[0].split('recoverAll')[0]
    print('journal boot-safe source contract: PASS')


def journal_parts():
    journal = (SRC / 'apps/M4xInstallJournal.cpp').read_text()
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
        'int recoverAll(',
    ]
    parts = []
    for n in names:
        body = function(journal, n)
        if n.startswith('struct ReconciledLoad'):
            body += ';'
        parts.append(body)
    return '\n'.join(parts)


def run_cpp():
    inc = json_inc()
    code = r'''
#include <cassert>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include "apps/M4xPaths.h"
#include "apps/M4xInstallTxn.h"
#include "apps/M4xInstallJournal.h"
#include <ArduinoJson.h>
struct Node { bool dir=false; std::string data; bool operator==(const Node& o) const { return dir==o.dir && data==o.data; } };
static std::map<std::string, Node> disk;
static std::set<std::string> unreadable;
static int journalWrites = 0;
static int dropCalls = 0;
static bool journalPath(const std::string& p) {
  const std::string j = M4xInstallTxn::kJournalPath;
  return p == j || p == j + ".tmp" || p == j + ".bak" || p == j + ".tmp.part";
}
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
    if (!disk.count(p) || disk[p].dir) return false;
    if (unreadable.count(p)) return false;
    f = FsFile(p);
    return true;
  }
  bool openFileForWrite(const char*, const char* p, FsFile& f) {
    if (disk.count(p) && disk[p].dir) return false;
    if (journalPath(p)) journalWrites++;
    disk[p] = Node{false,{}};
    f = FsFile(p);
    return true;
  }
  bool remove(const char* p) {
    if (journalPath(p)) journalWrites++;
    if (!disk.count(p) || disk[p].dir) return false;
    disk.erase(p);
    return true;
  }
  bool rename(const char* a, const char* b) {
    if (journalPath(a) || journalPath(b)) journalWrites++;
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
static std::string validBody(const char* id) {
  return std::string("{\"txns\":[{\"id\":\"") + id +
      "\",\"phase\":\"staging\",\"installPath\":\"/apps/" + id +
      "\",\"stagingPath\":\"/apps/" + id + ".staging\",\"backupPath\":\"/apps/" + id +
      ".bak\",\"hadPriorInstall\":false,\"newVersionCode\":1,\"newEntry\":\"main.lua\"}]}";
}
static bool dropStaging(const M4xInstallTxn::JournalRecord& rec, void*) {
  dropCalls++;
  disk.erase(rec.stagingPath);
  disk.erase(rec.stagingPath + "/main.lua");
  return true;
}
static bool existsPath(const std::string& p, void*) { return disk.count(p) > 0; }
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
int main() {
  const char* journal = M4xInstallTxn::kJournalPath;
  const std::string bak = std::string(journal) + ".bak";
  const std::string tmp = std::string(journal) + ".tmp";
  std::vector<M4xInstallTxn::JournalRecord> out;
  // 1. Healthy absent: empty success, recover 0, no hooks, no writes.
  {
    disk.clear(); unreadable.clear(); journalWrites = 0; dropCalls = 0;
    assert(M4xInstallJournal::tryLoadAll(out) && out.empty());
    assert(M4xInstallJournal::recoverAll(hooks()) == 0);
    assert(dropCalls == 0);
  }
  // 2. Healthy valid-empty: same, no failure.
  {
    disk.clear(); unreadable.clear(); journalWrites = 0; dropCalls = 0;
    disk[journal] = {false, "{\"txns\":[]}"};
    assert(M4xInstallJournal::tryLoadAll(out) && out.empty());
    assert(M4xInstallJournal::recoverAll(hooks()) == 0);
    assert(dropCalls == 0);
  }
  // 3. Present-but-unreadable primary + valid bak: fail closed, no stale promote.
  {
    disk.clear(); unreadable.clear(); journalWrites = 0; dropCalls = 0;
    disk[journal] = {false, validBody("keep")};
    disk[bak] = {false, "{\"txns\":[]}"};
    disk["/apps/keep.staging/main.lua"] = {false, "STAGE"};
    unreadable.insert(journal);
    auto before = disk;
    assert(!M4xInstallJournal::tryLoadAll(out));
    assert(M4xInstallJournal::recoverAll(hooks()) == 0);
    assert(dropCalls == 0 && journalWrites == 0 && disk == before);
    assert(disk[journal].data == validBody("keep"));
    assert(!M4xInstallJournal::upsert(stagingRec("x")));
    assert(disk == before);
  }
  // 4. Malformed selected snapshot (primary garbage, no valid fallback): fail closed.
  {
    disk.clear(); unreadable.clear(); journalWrites = 0; dropCalls = 0;
    disk[journal] = {false, "{not json"};
    auto before = disk;
    assert(!M4xInstallJournal::tryLoadAll(out));
    assert(M4xInstallJournal::recoverAll(hooks()) == 0);
    assert(dropCalls == 0 && journalWrites == 0 && disk == before);
  }
  // 5. Valid pending recovery still works.
  {
    disk.clear(); unreadable.clear(); journalWrites = 0; dropCalls = 0;
    disk[journal] = {false, validBody("pend")};
    disk["/apps/pend.staging/main.lua"] = {false, "STAGE"};
    assert(M4xInstallJournal::tryLoadAll(out) && out.size() == 1);
    assert(M4xInstallJournal::recoverAll(hooks()) == 1);
    assert(dropCalls == 1 && !disk.count("/apps/pend.staging/main.lua"));
    assert(M4xInstallJournal::tryLoadAll(out) && out.empty());
    (void)tmp;
  }
}
'''
    with tempfile.TemporaryDirectory(prefix='m4-bootsafe-') as t:
        cpp = Path(t) / 'test.cpp'
        exe = Path(t) / 'test'
        cpp.write_text(code)
        subprocess.run(
            ['c++', '-std=c++17', '-O1', '-g', '-fsanitize=address,undefined',
             '-fno-omit-frame-pointer',
             '-I', str(SRC), '-I', str(inc), '-I', str(inc / 'src'),
             str(cpp), '-o', str(exe)],
            check=True)
        subprocess.run([str(exe)], check=True)
    print('journal boot-safe fail-closed: PASS')


if __name__ == '__main__':
    test_source_contract()
    run_cpp()
