"""Write paths must fail closed when the primary registry cannot be read.

Compiles the real tryLoad/save bodies and the real install, uninstall, and
recovery commit hook. A present primary plus a failed read or Json NoMemory
must not save, and must not replace that primary with the empty table or the
older backup.
"""
from pathlib import Path
import subprocess
import tempfile

from test_m4_astra_stability import SRC, function

ROOT = Path(__file__).resolve().parents[2]
JSON_INC = ROOT / 'firmware/.pio/libdeps/murphy_m4/ArduinoJson'


def source_order():
    inst = (SRC / 'apps/M4xInstaller.cpp').read_text()
    for sig in (
        'bool hookCommitReg(',
        'M4xInstallResult M4xInstaller::install(',
        'bool M4xInstaller::uninstall(',
        'bool hookRegHas(',
        'bool hookRegMatch(',
    ):
        body = function(inst, sig)
        assert 'M4xRegistry::tryLoad' in body
        assert 'M4xRegistry::load' not in body
    commit = function(inst, 'bool hookCommitReg(')
    assert commit.index('tryLoad') < commit.index('M4xRegistry::save')
    install = function(inst, 'M4xInstallResult M4xInstaller::install(')
    assert install.index('tryLoad') < install.index('M4xRegistry::upsert')
    uninstall = function(inst, 'bool M4xInstaller::uninstall(')
    assert uninstall.index('tryLoad') < uninstall.index('M4xRegistry::remove')


def run():
    reg = (SRC / 'apps/M4xRegistry.cpp').read_text()
    inst = (SRC / 'apps/M4xInstaller.cpp').read_text()
    ns_start = reg.index('constexpr const char* kRegistryTmp')
    ns_end = reg.index('}  // namespace')
    registry = (
        'namespace {\n' + reg[ns_start:ns_end] + '\n}\n'
        + function(reg, 'std::vector<M4xInstalledApp> M4xRegistry::load(')
        + '\n' + function(reg, 'bool M4xRegistry::tryLoad(')
        + '\n' + function(reg, 'bool M4xRegistry::save(')
        + '\n' + function(reg, 'const M4xInstalledApp* M4xRegistry::find(')
        + '\n' + function(reg, 'void M4xRegistry::upsert(')
        + '\n' + function(reg, 'bool M4xRegistry::remove(')
    )
    hooks = '\n'.join([
        function(inst, 'bool refuseIfPendingJournal('),
        function(inst, 'bool hookCommitReg('),
        function(inst, 'M4xInstallResult M4xInstaller::install('),
        function(inst, 'bool M4xInstaller::uninstall('),
    ])
    code = r'''
#include <algorithm>
#include <cassert>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <ArduinoJson.h>
#include "apps/M4xPaths.h"
#include "apps/M4xRegistry.h"
#include "apps/M4xInstaller.h"
#include "apps/M4xPathSafe.h"
#include "apps/M4xInstallJournal.h"

static std::string gFailBody;
template <typename TDoc, typename TIn>
DeserializationError realDeserialize(TDoc& doc, const TIn& in) {
  return deserializeJson(doc, in);
}
template <typename TDoc, typename TIn>
DeserializationError deserializeJsonForced(TDoc& doc, const TIn& in) {
  if (!gFailBody.empty() && in == gFailBody) return DeserializationError::NoMemory;
  return realDeserialize(doc, in);
}
#define deserializeJson deserializeJsonForced

bool M4xParseRuntimeKind(const std::string&, M4xRuntimeKind& out) {
  out = M4xRuntimeKind::Lua;
  return true;
}
const char* M4xRuntimeKey(M4xRuntimeKind) { return "lua"; }
M4xManifest M4xParseManifest(const char*, size_t) { return {}; }
bool M4xIsValidPackageId(const std::string& id) { return id.find('.') != std::string::npos; }
uint32_t millis() { return 1000; }
struct EspShim { unsigned getFreeHeap() const { return 0; } } ESP;
struct SerialShim { template <class... A> void printf(const char*, A...) {} } Serial;
inline void resetTaskWdtIfSubscribed() {}
class InstallGateGuard {
 public:
  bool acquire() { return true; }
};

static bool gPrimaryReadFail = false;
static bool gBackupReadFail = false;
static int gUnexpected = 0;
struct Node { bool dir = false; std::string data; };
static std::map<std::string, Node> disk;
struct FsFile {
  std::string path; bool valid = false; size_t at = 0; bool writeErr = false;
  FsFile() = default;
  explicit FsFile(std::string p) : path(std::move(p)), valid(disk.count(path) && !disk[path].dir) {}
  explicit operator bool() const { return valid; }
  uint64_t fileSize() const { return valid ? disk.at(path).data.size() : 0; }
  int read(uint8_t* p, size_t n) {
    if (!valid) return -1;
    auto& data = disk.at(path).data;
    if (at >= data.size()) return 0;
    size_t nread = std::min(n, data.size() - at);
    memcpy(p, data.data() + at, nread);
    at += nread;
    return static_cast<int>(nread);
  }
  int write(const uint8_t* p, size_t n) {
    if (!valid) return -1;
    disk[path].data.append(reinterpret_cast<const char*>(p), n);
    return static_cast<int>(n);
  }
  bool getWriteError() const { return writeErr; }
  bool sync() { return true; }
  bool close() { valid = false; return true; }
};
struct SdShim {
  bool exists(const char* p) const { return disk.count(p); }
  bool mkdir(const char* p, bool) { disk[p] = Node{true, {}}; return true; }
  bool openFileForRead(const char*, const char* p, FsFile& f) {
    if (gPrimaryReadFail && std::string(p) == M4xPaths::kRegistryPath) return false;
    if (gBackupReadFail && std::string(p) == "/system/app_registry.json.bak") return false;
    if (!disk.count(p) || disk[p].dir) return false;
    f = FsFile(p);
    return true;
  }
  bool openFileForWrite(const char*, const char* p, FsFile& f) {
    if (disk.count(p) && disk[p].dir) return false;
    disk[p] = Node{false, {}};
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
    disk[b] = disk[a];
    disk.erase(a);
    return true;
  }
  bool removeDir(const char*) { ++gUnexpected; return false; }
} SdMan;

void removeTreeBestEffort(const std::string&, const M4xManifest*) { ++gUnexpected; }
bool extractListed(const std::string&, const std::string&, const M4xManifest&, M4xInstallResult&) {
  ++gUnexpected; return false;
}
M4xManifest manifestFromInventory(const std::string&, const std::string&, const std::vector<std::string>&) {
  ++gUnexpected; return {};
}
M4xInstallTxn::JournalRecord makeJournalBase(const M4xManifest&, const std::string&, const std::string&,
                                             const std::string&, const M4xInstalledApp*) {
  ++gUnexpected; return {};
}
bool journalSetPhase(M4xInstallTxn::JournalRecord&, M4xInstallTxn::Phase) { ++gUnexpected; return false; }
bool promoteStaging(const std::string&, const std::string&, const std::string&, const M4xManifest&,
                    const M4xManifest*, std::string&) {
  ++gUnexpected; return false;
}
namespace M4xInstallJournal {
bool readPending(const std::string&, bool& pending) { pending = false; return true; }
bool upsert(const M4xInstallTxn::JournalRecord&) { ++gUnexpected; return false; }
bool remove(const std::string&) { ++gUnexpected; return false; }
}  // namespace M4xInstallJournal

M4xInstallResult M4xInstaller::probe(const std::string&) {
  M4xInstallResult r;
  r.ok = true;
  r.manifest.id = "com.example.new";
  r.manifest.name = "N";
  r.manifest.version = "9";
  r.manifest.versionCode = 9;
  r.manifest.entry = "main.lua";
  r.manifest.valid = true;
  r.installPath = "/apps/com.example.new";
  return r;
}
''' + registry + '\n' + hooks + r'''
static const char* kBak = "/system/app_registry.json.bak";
static const std::string kNew =
    "{\"apps\":[{\"id\":\"com.example.new\",\"name\":\"N\",\"version\":\"2\",\"versionCode\":2,"
    "\"path\":\"/apps/com.example.new\",\"entry\":\"main.lua\"}]}";
static const std::string kOld =
    "{\"apps\":[{\"id\":\"com.example.old\",\"name\":\"O\",\"version\":\"1\",\"versionCode\":1,"
    "\"path\":\"/apps/com.example.old\",\"entry\":\"main.lua\"}]}";

static void plant(const std::string& primary) {
  disk.clear();
  gUnexpected = 0;
  gPrimaryReadFail = false;
  gBackupReadFail = false;
  gFailBody.clear();
  disk[M4xPaths::kRegistryPath] = Node{false, primary};
  disk[kBak] = Node{false, kOld};
}

static void expectUnchanged() {
  assert(disk[M4xPaths::kRegistryPath].data == kNew);
  assert(disk[kBak].data == kOld);
  assert(gUnexpected == 0);
  assert(!disk.count("/system/app_registry.json.tmp"));
}

static void rejectWritePaths() {
  expectUnchanged();
  auto ui = M4xRegistry::load();
  assert(ui.size() == 1 && ui[0].id == "com.example.old");
  expectUnchanged();
  std::vector<M4xInstalledApp> apps;
  assert(!M4xRegistry::tryLoad(apps));
  assert(apps.empty());
  expectUnchanged();
  M4xInstallTxn::JournalRecord rec;
  rec.id = "com.example.new";
  rec.installPath = "/apps/com.example.new";
  rec.newVersionCode = 2;
  rec.newEntry = "main.lua";
  assert(!hookCommitReg(rec, nullptr));
  expectUnchanged();
  std::string err;
  assert(!M4xInstaller::uninstall("com.example.new", false, err));
  assert(err == "registry_read");
  expectUnchanged();
  M4xInstallResult installed = M4xInstaller::install("/inbox/app.m4x");
  assert(!installed.ok && installed.error == "registry_read");
  expectUnchanged();
}

// Real M4xRegistry::tryLoad + installer hook must fail closed if the ONLY
// surviving backup cannot be read or its JSON parse runs out of memory.
static void rejectUnreadableBackup(bool missingPrimary, bool backupIoError) {
  plant("{");  // a confirmed-corrupt primary; optionally remove it entirely.
  if (missingPrimary) disk.erase(M4xPaths::kRegistryPath);
  gBackupReadFail = backupIoError;
  if (!backupIoError) gFailBody = kOld;  // force backup JSON NoMemory
  const auto originalDisk = disk;
  std::vector<M4xInstalledApp> apps;
  assert(!M4xRegistry::tryLoad(apps));
  assert(apps.empty());
  M4xInstallTxn::JournalRecord rec;
  rec.id = "com.example.new";
  rec.installPath = "/apps/com.example.new";
  rec.newVersionCode = 2;
  rec.newEntry = "main.lua";
  assert(!hookCommitReg(rec, nullptr));
  std::string err;
  assert(!M4xInstaller::uninstall("com.example.new", false, err));
  assert(err == "registry_read");
  M4xInstallResult installed = M4xInstaller::install("/inbox/app.m4x");
  assert(!installed.ok && installed.error == "registry_read");
  assert(disk.size() == originalDisk.size());
  assert(disk.at(kBak).data == kOld);
  if (!missingPrimary) {
    assert(disk.at(M4xPaths::kRegistryPath).data == "{");
  } else {
    assert(!disk.count(M4xPaths::kRegistryPath));
  }
  assert(!disk.count("/system/app_registry.json.tmp"));
  assert(gUnexpected == 0);
  gBackupReadFail = false;
  gFailBody.clear();
}

int main() {
  rejectUnreadableBackup(false, false); // corrupt primary, backup NoMemory
  rejectUnreadableBackup(true, false);  // missing primary, backup NoMemory
  rejectUnreadableBackup(false, true);  // corrupt primary, backup SD read error
  rejectUnreadableBackup(true, true);   // missing primary, backup SD read error
  disk.clear();
  gUnexpected = 0;
  std::vector<M4xInstalledApp> firstInstall;
  assert(M4xRegistry::tryLoad(firstInstall) && firstInstall.empty());

  plant(kNew);
  gPrimaryReadFail = true;
  rejectWritePaths();

  plant(kNew);
  gFailBody = kNew;
  rejectWritePaths();

  plant(kNew);
  std::vector<M4xInstalledApp> apps;
  assert(M4xRegistry::tryLoad(apps));
  assert(apps.size() == 1 && apps[0].id == "com.example.new");
  assert(M4xRegistry::save(apps));
  assert(disk[kBak].data == kNew);
  assert(disk[M4xPaths::kRegistryPath].data.find("com.example.new") != std::string::npos);
  apps.clear();
  assert(M4xRegistry::tryLoad(apps));
  assert(apps.size() == 1 && apps[0].id == "com.example.new");

  plant("{");
  const std::string bakBefore = disk[kBak].data;
  assert(M4xRegistry::tryLoad(apps));
  assert(apps.size() == 1 && apps[0].id == "com.example.old");
  assert(disk[M4xPaths::kRegistryPath].data == kOld);
  assert(disk[kBak].data == bakBefore);
  assert(gUnexpected == 0);
}
'''
    with tempfile.TemporaryDirectory(prefix='m4-reg-status-') as tmp:
        cpp = Path(tmp) / 'test.cpp'
        exe = Path(tmp) / 'test'
        cpp.write_text(code)
        subprocess.run(
            ['c++', '-std=c++17', '-O1', '-g', '-fsanitize=address,undefined',
             '-fno-omit-frame-pointer', '-I', str(SRC), '-I', str(JSON_INC),
             '-I', str(JSON_INC / 'src'), str(cpp), '-o', str(exe)],
            check=True)
        subprocess.run([str(exe)], check=True)
    print('registry status propagation: PASS')


if __name__ == '__main__':
    source_order()
    run()
