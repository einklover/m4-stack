"""Compile actual firmware method bodies against fault-injecting host shims.
No SD image, hardware, network or installed Python test framework required.
"""
from pathlib import Path
import subprocess
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

def run(code, name):
    with tempfile.TemporaryDirectory(prefix='m4-astra-host-') as tmp:
        cpp = Path(tmp)/'test.cpp'; exe = Path(tmp)/'test'
        cpp.write_text(code)
        subprocess.run(['c++', '-std=c++17', '-O1', '-g', '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', str(cpp), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print(name + ': PASS')

COMMON = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>
'''

def workers():
    code = COMMON + r'''
static int live = 0, deleted = 0;
struct Resource { std::vector<int> data = std::vector<int>(1000); Resource(){++live;} ~Resource(){--live;} };
namespace M4Psram { void deleteTask(void*) { assert(live == 0); ++deleted; } }
'''
    for name in ['Catalog', 'Discovery', 'Login', 'BookDetailAsync']:
        s = (SRC / f'apps/providers/M4NativeProvider{name}.cpp').read_text()
        assert 'void runJob()' in s, name
        task = function(s, 'void taskMain(void*)')
        assert task.index('runJob();') < task.index('gBusy.store(false') < task.index('M4Psram::deleteTask')
        if 'gTask = nullptr' in task:
            assert task.index('gTask = nullptr') < task.index('gBusy.store(false')
        code += 'namespace ' + name + r''' {
std::atomic<bool> gBusy{true}; std::mutex gMu; void* gTask=nullptr;
void runJob() { Resource owned; return; }
''' + task + '\n}\n'
    code += 'int main() { for(int i=0;i<1000;++i) {'
    for name in ['Catalog', 'Discovery', 'Login', 'BookDetailAsync']:
        code += f'{name}::taskMain(nullptr); assert(!{name}::gBusy.load());\n'
    code += '} assert(deleted==4000 && live==0); }'
    run(code, 'actual task completion wrappers / 4000 early returns')
    store = function((SRC/'activities/apps/AppStoreActivity.cpp').read_text(), 'void AppStoreActivity::taskEntry')
    assert store.count('M4Psram::deleteTask(nullptr)') == 1
    assert store.index('run();') < store.index('job.reset();') < store.index('s->busy.store(false') < store.index('s.reset();') < store.index('M4Psram::deleteTask')

def library():
    s = (SRC/'activities/home/MyLibraryActivity.cpp').read_text()
    actual = s[s.index('void MyLibraryActivity::loadFiles()'):s.index('//enter也需要改')]
    code = COMMON + r'''
static uint32_t now=0; uint32_t millis(){return now;}
unsigned uxTaskGetStackHighWaterMark(void*){return 0;}
struct SerialShim { template<class... A> void printf(const char*, A... ) {} } Serial;
struct Entry {std::string name; bool directory=false; uint32_t size=1;};
static std::vector<Entry> disk;
static int failAfter=-1, skewAt=-1, reads=0, opens=0, openChildren=0;
struct FsFile {
 bool valid=false, root=false; uint64_t position=0; size_t item=0; uint8_t error=0;
 FsFile() = default;
 FsFile(const FsFile& o) { *this = o; }
 FsFile& operator=(const FsFile& o) {
   if (this == &o) return *this;
   close();
   valid=o.valid; root=o.root; position=o.position; item=o.item; error=o.error;
   if (valid && !root) ++openChildren;
   return *this;
 }
 FsFile(FsFile&& o) noexcept { *this = std::move(o); }
 FsFile& operator=(FsFile&& o) noexcept {
   if (this == &o) return *this;
   close();
   valid=o.valid; root=o.root; position=o.position; item=o.item; error=o.error;
   o.valid=false;
   return *this;
 }
 ~FsFile() { close(); }
 explicit operator bool() const { return valid; }
 bool isDirectory() const { return root || (item < disk.size() && disk[item].directory); }
 // FAT and exFAT directory cursors are 32-byte slots, not dense item indexes.
 bool seekSet(uint64_t p) {
   if (!valid || !root || (p & 31ull) || p / 32 > disk.size()) return false;
   position = p;
   return true;
 }
 uint64_t curPosition() const { return position; }
 void close() {
   if (!valid) return;
   if (root) assert(openChildren == 0);
   else --openChildren;
   valid = false;
 }
 int getError() const { return error; }
 FsFile openNextFile() {
   ++reads; ++now;
   if (!valid || !root) return {};
   if ((position & 31ull) || (failAfter >= 0 && reads >= failAfter)) { error = 1; return {}; }
   const size_t index = static_cast<size_t>(position / 32);
   if (index >= disk.size()) return {};
   FsFile f; f.valid = true; f.item = index; ++openChildren; position += 32;
   if (skewAt >= 0 && reads == skewAt) position += 1; // one bad slot, then later reads stay aligned
   return f;
 }
 size_t getName(char* p, size_t n) {
   if (!n || item >= disk.size()) return 0;
   const auto& name = disk[item].name;
   if (name.empty() || name.size() >= n) { p[0] = 0; return 0; } // SdFat: truncation is 0, not a short count.
   memcpy(p, name.data(), name.size()); p[name.size()] = 0; return name.size();
 }
 uint64_t size() const { return item < disk.size() ? disk[item].size : 0; }
};
struct SdShim { FsFile open(const char*) { ++opens; FsFile f; f.valid = f.root = true; return f; } } SdMan;
namespace StringUtils { bool checkFileExtension(const std::string& a, const char* b) { size_t n=strlen(b); return a.size()>=n && a.compare(a.size()-n, n, b)==0; } }
struct MyLibraryActivity {
 std::string basepath="/"; std::vector<std::string> files; std::vector<uint32_t> fileSizes;
 FsFile scanDirectory; uint64_t directoryPageStart=0, directoryNextStart=0, directoryCursor=0;
 bool directoryLoading=false, directoryHasMore=false, directoryError=false;
 size_t directoryNameBytes=0, directoryVisited=0, selectorIndex=0; uint32_t directoryStartedMs=0;
 std::string directoryPendingSelect;
 bool isSearchMode=false, showAllFiles=false, updateRequired=false;
 size_t findEntry(const std::string& name) const {
   for (size_t i = 0; i < files.size(); ++i) if (files[i] == name) return i;
   return 0;
 }
 void loadFiles(); void startDirectoryPage(uint64_t); void scanDirectoryBatch(); void finishDirectoryPage(); bool selectDirectoryPage();
};
''' + actual + r'''
void settle(MyLibraryActivity& a) {
 int guard = 0;
 while (a.directoryLoading) {
   int before = reads;
   a.scanDirectoryBatch();
   assert(reads - before <= 16);
   assert(!a.scanDirectory);          // handle is closed even when the page is unfinished
   assert(openChildren == 0);
   assert(++guard < 20000);
 }
 assert(!a.scanDirectory);
 assert(a.files.size() <= 66);
 assert(a.directoryNameBytes <= 8192);
}
void exercise(size_t count, bool longNames) {
 disk.clear(); reads = 0; failAfter = -1; skewAt = -1; openChildren = 0;
 for (size_t i = 0; i < count; ++i) {
   char n[30]; snprintf(n, sizeof(n), "%08zu.txt", i);
   disk.push_back({(longNames ? std::string(500, 'x') : std::string()) + n, false, uint32_t(i + 1)});
 }
 MyLibraryActivity a; a.loadFiles(); std::set<uint32_t> seen; size_t pages = 0;
 while (true) {
   settle(a); ++pages;
   for (auto n : a.fileSizes) if (n) assert(seen.insert(n).second);
   if (!a.directoryHasMore) break;
   a.selectorIndex = a.files.size() - 1;
   assert(a.selectDirectoryPage());
 }
 assert(seen.size() == count);
 assert(reads <= int(count + pages + 1));
 if (pages > 1) { a.selectorIndex = 0; assert(a.selectDirectoryPage()); settle(a); assert(a.directoryPageStart == 0); assert(a.files[0] != "< 回到首批 >"); }
}
int main() {
 exercise(0, false); exercise(64, false); exercise(65, false); exercise(1000, false); exercise(10000, false); exercise(10000, true);
 disk.assign(10000, {"hidden.bin", false, 1});
 MyLibraryActivity a; a.loadFiles(); a.scanDirectoryBatch();
 assert(a.directoryLoading && !a.scanDirectory && openChildren == 0);
 a.startDirectoryPage(0); assert(!a.scanDirectory); settle(a); assert(a.files.empty() && !a.directoryError);
 disk.assign(100, {"book.txt", false, 1}); reads = 0; failAfter = 4; a.loadFiles(); settle(a);
 assert(a.directoryError && a.files.empty());
 failAfter = -1; disk = {{std::string(900, 'x'), false, 1}, {"ok.txt", false, 7}};
 a.loadFiles(); settle(a);
 assert(!a.directoryError && a.files.size() == 1 && a.files[0] == "ok.txt");
 disk.clear();
 for (int i = 0; i < 10; ++i) { char n[16]; snprintf(n, sizeof(n), "%02d.txt", i); disk.push_back({n, false, uint32_t(i + 1)}); }
 MyLibraryActivity b; b.loadFiles(); b.directoryPendingSelect = "07.txt"; settle(b);
 assert(b.directoryPendingSelect.empty() && b.selectorIndex < b.files.size() && b.files[b.selectorIndex] == "07.txt");
 b.startDirectoryPage(1); settle(b); assert(b.directoryError && b.files.empty());
 // A partial batch that ends off a 32-byte slot must fail closed. Leaving the
 // old cursor would scan the same names on every UI turn.
 disk.assign(100, {"book.txt", false, 1}); reads = 0; failAfter = -1; skewAt = 4; openChildren = 0;
 MyLibraryActivity c; c.loadFiles(); settle(c);
 assert(c.directoryError && c.files.empty() && !c.directoryLoading && !c.scanDirectory);
 skewAt = -1;
}
'''
    run(code, 'actual directory scanner / 1k+10k+10k long names+I/O faults')
    assert 'xTaskCreate(&MyLibraryActivity' not in s
    assert 'deleteFileOrDir(copySourcePath)' not in s
    assert 'O_WRONLY | O_CREAT | O_EXCL' in s
    copy = s[s.index('bool copyFile'):s.index('// Search is bounded')]
    assert 'srcFile.close()' in copy[copy.index('if (!dstFile)'):copy.index('const bool ok')]
    ret = s[s.index('void MyLibraryActivity::returnToParent()'):s.index('void MyLibraryActivity::returnToRoot()')]
    assert ret.index('startDirectoryPage') < ret.index('directoryPendingSelect')
    assert 'listSize > 0 && upReleased' in s
    loop = s[s.index('void MyLibraryActivity::loop()'):]
    loading = loop[loop.index('if (directoryLoading)'):loop.index('if (subActivity)')]
    assert 'returnToParent()' in loading

if __name__ == '__main__':
    workers()
    library()
