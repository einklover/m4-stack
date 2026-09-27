"""Fault injection for registry save and journal replace.

Compiles the real writer bodies. The in-memory SD truncates on
openFileForWrite, matching O_RDWR|O_CREAT|O_TRUNC. A create-only writer
must refuse an existing path instead of opening it.
"""
from pathlib import Path
import subprocess
import tempfile
from test_m4_astra_stability import SRC, COMMON, function

ROOT = Path(__file__).resolve().parents[2]
JSON_INC = ROOT / 'firmware/.pio/libdeps/murphy_m4/ArduinoJson'


def source_order():
    inst = (SRC / 'apps/M4xInstaller.cpp').read_text()
    install = function(inst, 'M4xInstallResult M4xInstaller::install(')
    uninstall = function(inst, 'bool M4xInstaller::uninstall(')
    ensure = function(inst, 'void M4xInstaller::ensureLayout()')
    assert install.index('gate.acquire') < install.index('probe(')
    assert 'storageMutex_' not in install and 'gM4RenderMutex' not in install
    assert uninstall.index('gate.acquire') < uninstall.index('M4xIsValidPackageId')
    assert 'storageMutex_' not in uninstall and 'gM4RenderMutex' not in uninstall
    assert 'installGate' not in ensure and 'gate.acquire' not in ensure
    reg = (SRC / 'apps/M4xRegistry.cpp').read_text()
    save = function(reg, 'bool M4xRegistry::save(')
    assert save.index('doc.overflowed()') < save.index('writeAllTextExact')
    assert save.index('written != out.size()') < save.index('SdMan.mkdir')
    assert save.index('bakCheck') < save.index('SdMan.remove(M4xPaths::kRegistryPath)')
    journal = (SRC / 'apps/M4xInstallJournal.cpp').read_text()
    durable = function(journal, 'bool durableWriteJournal(')
    save_all = function(journal, 'bool saveAll(')
    assert durable.index('writeExactFile') < durable.index('SdMan.remove(tmp')
    assert durable.index('writeExactFile') < durable.index('SdMan.remove(path)')
    assert save_all.index('doc.overflowed()') < save_all.index('durableWriteJournal')
    assert save_all.index('written != out.size()') < save_all.index('SdMan.mkdir')


def run_io(code, name):
    with tempfile.TemporaryDirectory(prefix='m4-reg-') as tmp:
        cpp = Path(tmp) / 'test.cpp'
        exe = Path(tmp) / 'test'
        cpp.write_text(code)
        subprocess.run(
            ['c++', '-std=c++17', '-O1', '-g', '-fsanitize=address,undefined',
             '-fno-omit-frame-pointer', '-I', str(SRC), '-I', str(JSON_INC),
             '-I', str(JSON_INC / 'src'),
             str(cpp), '-o', str(exe)],
            check=True)
        subprocess.run([str(exe)], check=True)
    print(name + ': PASS')


def bodies():
    reg = (SRC / 'apps/M4xRegistry.cpp').read_text()
    journal = (SRC / 'apps/M4xInstallJournal.cpp').read_text()
    parts = [
        function(reg, 'std::string readAllText('),
        function(reg, 'bool writeAllTextExact('),
        function(reg, 'bool M4xRegistry::save('),
        function(journal, 'bool readExactFile('),
        function(journal, 'bool writeExactFile('),
        function(journal, 'bool copyFileExact('),
        function(journal, 'bool renameOrCopy('),
        function(journal, 'bool isValidJournalBody('),
        function(journal, 'void pushStringArray('),
        function(journal, 'void writeOne('),
        function(journal, 'bool durableWriteJournal('),
        function(journal, 'std::string loadReconciledRaw('),
        function(journal, 'bool saveAll('),
    ]
    code = COMMON + r'''
#include <map>
#include <utility>
#include <cstdint>
#include "apps/M4xPaths.h"
#include "apps/M4xRegistry.h"
#include "apps/M4xInstallTxn.h"
#include <ArduinoJson.h>
const char* M4xRuntimeKey(M4xRuntimeKind){return "lua";}
constexpr const char* kRegistryTmp = "/system/app_registry.json.tmp";
constexpr const char* kRegistryBak = "/system/app_registry.json.bak";
struct Node {bool dir=false;std::string data;};
static std::map<std::string,Node> disk;
static std::set<std::string> removeBlocked;
static bool syncOk=true,closeOk=true,shortWrite=false,renameOk=true;
struct FsFile {
  std::string path;bool valid=false;size_t at=0;bool writeErr=false;
  FsFile()=default;
  explicit FsFile(std::string p):path(std::move(p)),valid(disk.count(path)&&!disk[path].dir){}
  explicit operator bool()const{return valid;}
  uint64_t fileSize()const{return valid?disk.at(path).data.size():0;}
  int read(uint8_t* p,size_t n){
    if(!valid)return -1;
    auto& data=disk.at(path).data;
    if(at>=data.size())return 0;
    size_t nread=std::min(n,data.size()-at);
    memcpy(p,data.data()+at,nread);
    at+=nread;
    return static_cast<int>(nread);
  }
  int write(const uint8_t* p,size_t n){
    if(!valid)return -1;
    if(shortWrite){writeErr=true;size_t out=n/2;if(out==0)return 0;disk[path].data.append(reinterpret_cast<const char*>(p),out);return static_cast<int>(out);}
    disk[path].data.append(reinterpret_cast<const char*>(p),n);
    return static_cast<int>(n);
  }
  bool getWriteError()const{return writeErr;}
  bool sync(){return syncOk;}
  bool close(){valid=false;return closeOk;}
};
struct SdShim {
  bool exists(const char* p)const{return disk.count(p);}
  bool mkdir(const char* p,bool){disk[p]=Node{true,{}};return true;}
  bool openFileForRead(const char*,const char* p,FsFile& f){
    if(!disk.count(p)||disk[p].dir)return false;
    f=FsFile(p);return true;
  }
  bool openFileForWrite(const char*,const char* p,FsFile& f){
    if(disk.count(p)&&disk[p].dir)return false;
    disk[p]=Node{false,{}};
    f=FsFile(p);return true;
  }
  bool remove(const char* p){
    if(removeBlocked.count(p)||!disk.count(p)||disk[p].dir)return false;
    disk.erase(p);return true;
  }
  bool rename(const char* a,const char* b){
    if(!renameOk||!disk.count(a)||disk[a].dir||disk.count(b))return false;
    disk[b]=disk[a];disk.erase(a);return true;
  }
} SdMan;
void reset(){disk.clear();removeBlocked.clear();syncOk=closeOk=renameOk=true;shortWrite=false;}
M4xInstalledApp sample(){M4xInstalledApp a;a.id="com.example.clock";a.name="Clock";a.version="1.0";a.versionCode=1;a.path="/apps/com.example.clock";a.entry="main.lua";return a;}
''' + '\n'.join(parts) + r'''
static const char* kPrimary=M4xPaths::kRegistryPath;
static const char* kJournal=M4xInstallTxn::kJournalPath;
int main(){
  const std::string oldReg="{\"apps\":[{\"id\":\"keep-me\"}]}";
  const std::string sole="{\"txns\":[{\"id\":\"kept\"}]}";
  const std::string newer="{\"txns\":[{\"id\":\"newer\"}]}";
  const std::string part=std::string(kJournal)+".tmp.part";
  const std::string jtmp=std::string(kJournal)+".tmp";

  reset();
  disk["/already"]={false,"KEEP"};
  assert(!writeExactFile("/already","NEW"));
  assert(disk["/already"].data=="KEEP");
  assert(!writeAllTextExact("/already","NEW"));
  assert(disk["/already"].data=="KEEP");

  reset();
  disk[kPrimary]={false,oldReg};
  syncOk=false;
  assert(!M4xRegistry::save({sample()}));
  assert(disk[kPrimary].data==oldReg);
  assert(!disk.count(kRegistryTmp));

  reset();
  disk[kPrimary]={false,oldReg};
  shortWrite=true;
  assert(!M4xRegistry::save({sample()}));
  assert(disk[kPrimary].data==oldReg);
  assert(!disk.count(kRegistryTmp));

  reset();
  disk[kPrimary]={false,oldReg};
  disk[kRegistryBak]={false,"OLD-BAK"};
  renameOk=false;
  removeBlocked.insert(kRegistryBak);
  assert(!M4xRegistry::save({sample()}));
  assert(disk[kPrimary].data==oldReg);
  assert(disk[kRegistryBak].data=="OLD-BAK");
  assert(!disk.count(kRegistryTmp));

  reset();
  disk[kPrimary]={false,oldReg};
  renameOk=false;
  removeBlocked.insert(kPrimary);
  assert(!M4xRegistry::save({sample()}));
  assert(disk.count(kPrimary));
  assert(disk[kPrimary].data==oldReg);

  reset();
  disk[kPrimary]={false,oldReg};
  renameOk=false;
  assert(M4xRegistry::save({sample()}));
  assert(disk[kPrimary].data.find("com.example.clock")!=std::string::npos);
  assert(!disk.count(kRegistryTmp));

  reset();
  disk[kPrimary]={false,oldReg};
  assert(M4xRegistry::save({sample()}));
  assert(disk[kPrimary].data.find("com.example.clock")!=std::string::npos);
  assert(disk.count(kRegistryBak));
  assert(disk[kRegistryBak].data==oldReg);
  assert(!disk.count(kRegistryTmp));

  reset();
  disk[jtmp]={false,sole};
  assert(!durableWriteJournal(kJournal,"not-json"));
  assert(disk[jtmp].data==sole);
  assert(!disk.count(part));
  assert(!disk.count(kJournal));

  reset();
  disk[jtmp]={false,sole};
  syncOk=false;
  assert(!durableWriteJournal(kJournal,newer));
  assert(disk[jtmp].data==sole);
  assert(!disk.count(part));
  assert(!disk.count(kJournal));

  reset();
  disk[jtmp]={false,sole};
  shortWrite=true;
  assert(!durableWriteJournal(kJournal,newer));
  assert(disk[jtmp].data==sole);
  assert(!disk.count(part));

  reset();
  disk[jtmp]={false,sole};
  disk[part]={false,"STALE"};
  removeBlocked.insert(part);
  assert(!durableWriteJournal(kJournal,newer));
  assert(disk[jtmp].data==sole);
  assert(disk[part].data=="STALE");

  reset();
  disk[jtmp]={false,sole};
  assert(durableWriteJournal(kJournal,newer));
  assert(disk[kJournal].data==newer);
  assert(!disk.count(jtmp));
  assert(!disk.count(part));

  reset();
  disk[jtmp]={false,sole};
  assert(loadReconciledRaw()==sole);
  assert(disk[kJournal].data==sole);
  assert(!disk.count(jtmp));
  assert(!disk.count(part));

  reset();
  assert(saveAll({}));
  assert(disk[kJournal].data.find("\"txns\"")!=std::string::npos);
  assert(!disk.count(jtmp));
  assert(!disk.count(part));
}
'''
    run_io(code, 'actual registry and journal writers / sole tmp, sync, rename, short write')


if __name__ == '__main__':
    source_order()
    print('registry/journal source order: PASS')
    bodies()
