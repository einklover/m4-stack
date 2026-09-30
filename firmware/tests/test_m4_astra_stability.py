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
static int failAfter=-1, skewAt=-1, reads=0, opens=0, openChildren=0, seekCalls=0, fatSteps=0;
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
 // SdFat 2.3.1 FatFile::seekSet: equal position is free. A reopened file has
 // position 0 and follows the FAT chain from the first cluster. 4096-byte
 // clusters make that prefix walk visible without a real card.
 bool seekSet(uint64_t p) {
   if (!valid || !root || (p & 31ull) || p / 32 > disk.size()) return false;
   ++seekCalls;
   if (p != position) {
     constexpr unsigned kShift = 12;
     uint32_t nNew = p == 0 ? 0u : (uint32_t)((p - 1) >> kShift);
     uint32_t nCur = position == 0 ? 0u : (uint32_t)((position - 1) >> kShift);
     if (p != 0 && (nNew < nCur || position == 0)) fatSteps += (int)nNew;
     else if (nNew > nCur) fatSteps += (int)(nNew - nCur);
     position = p;
   }
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
 const int opensBefore = opens, seeksBefore = seekCalls;
 while (a.directoryLoading) {
   int before = reads;
   a.scanDirectoryBatch();
   assert(reads - before <= 16);
   // The old assertion required !scanDirectory after every turn. That matched
   // the close/reopen implementation and forced the next batch to seekSet from
   // position 0. An in-progress page now keeps one directory cursor; child
   // handles are still closed before the turn returns.
   assert(openChildren == 0);
   if (a.directoryLoading) assert(static_cast<bool>(a.scanDirectory));
   else assert(!a.scanDirectory);
   assert(opens == opensBefore + 1);
   if (!a.directoryError) assert(seekCalls == seeksBefore + 1);
   assert(++guard < 20000);
 }
 assert(!a.scanDirectory);
 assert(a.files.size() <= 66);
 assert(a.directoryNameBytes <= 8192);
}
int pageSeekCost(size_t count, size_t perPage) {
 int steps = 0;
 if (!count || !perPage) return 0;
 for (size_t done = 0; done < count; ) {
   uint64_t p = done * 32ull;
   if (p) steps += (int)((p - 1) >> 12);
   done += std::min(perPage, count - done);
 }
 return steps;
}
int reopenEveryBatchCost(size_t count) {
 constexpr int kBatch = 4; // host clock: openNext bumps millis by 1, budget is 4
 int steps = 0;
 int batches = (int)((count + kBatch - 1) / kBatch);
 for (int i = 0; i < batches; ++i) {
   uint64_t p = (uint64_t)i * kBatch * 32ull;
   if (p) steps += (int)((p - 1) >> 12);
 }
 return steps;
}
void exercise(size_t count, bool longNames) {
 disk.clear(); reads = 0; failAfter = -1; skewAt = -1; openChildren = 0;
 opens = seekCalls = fatSteps = 0;
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
 size_t perPage = 0, nameBytes = 0;
 while (perPage < count && perPage < 64 && nameBytes + disk[perPage].name.size() + 2 <= 8192) {
   nameBytes += disk[perPage].name.size() + 2;
   ++perPage;
 }
 assert(opens == (int)pages && seekCalls == (int)pages);
 assert(fatSteps == pageSeekCost(count, perPage));
 if (count >= 1000) assert(fatSteps * 2 < reopenEveryBatchCost(count));
 if (pages > 1) { a.selectorIndex = 0; assert(a.selectDirectoryPage()); settle(a); assert(a.directoryPageStart == 0); assert(a.files[0] != "< 回到首批 >"); assert(!a.scanDirectory && openChildren == 0); }
}
void filtered(size_t count, bool longNames) {
 disk.clear(); reads = 0; failAfter = -1; skewAt = -1; openChildren = 0;
 opens = seekCalls = fatSteps = 0;
 for (size_t i = 0; i < count; ++i) {
   char n[30]; snprintf(n, sizeof(n), "%08zu.bin", i);
   disk.push_back({(longNames ? std::string(500, 'x') : std::string()) + n, false, 1});
 }
 MyLibraryActivity a; a.loadFiles(); a.scanDirectoryBatch();
 assert(a.directoryLoading && static_cast<bool>(a.scanDirectory) && openChildren == 0);
 assert(opens == 1 && seekCalls == 1 && reads > 0 && reads <= 16);
 while (a.directoryLoading) {
   int before = reads;
   a.scanDirectoryBatch();
   assert(reads - before <= 16 && openChildren == 0);
   assert(opens == 1 && seekCalls == 1);
   if (a.directoryLoading) assert(static_cast<bool>(a.scanDirectory));
 }
 assert(!a.scanDirectory && a.files.empty() && !a.directoryError && fatSteps == 0);
 const int quadratic = reopenEveryBatchCost(count);
 assert(quadratic > fatSteps);
 if (count >= 1000) assert(quadratic >= 64);
}
int main() {
 exercise(0, false); exercise(64, false); exercise(65, false); exercise(1000, false); exercise(10000, false); exercise(10000, true);
 filtered(1000, false); filtered(10000, false); filtered(1000, true); filtered(10000, true);
 disk.assign(10000, {"hidden.bin", false, 1}); reads = 0; opens = 0; seekCalls = 0; fatSteps = 0; openChildren = 0;
 MyLibraryActivity a; a.loadFiles(); a.scanDirectoryBatch();
 // One turn of an unfinished filtered page keeps the cursor. The previous test
 // required it to be closed, which is the quadratic reopen this case rejects.
 assert(a.directoryLoading && static_cast<bool>(a.scanDirectory) && openChildren == 0 && opens == 1);
 a.startDirectoryPage(0); assert(!a.scanDirectory); opens = seekCalls = fatSteps = 0; settle(a); assert(a.files.empty() && !a.directoryError && opens == 1);
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
 disk.assign(80, {"hidden.bin", false, 1}); reads = 0; opens = 0; seekCalls = 0; fatSteps = 0; openChildren = 0; failAfter = -1;
 MyLibraryActivity d; d.loadFiles(); d.scanDirectoryBatch();
 assert(d.directoryLoading && d.scanDirectory && opens == 1 && openChildren == 0);
 d.scanDirectory.close();
 d.directoryLoading = false;
 assert(!d.scanDirectory && !d.directoryLoading && openChildren == 0);
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
    # Cancel and activity exit close the retained cursor. Those functions sit
    # outside the extracted scanner, so the order is checked on the real source.
    # The compiled test runs the same two cancel statements after one open.
    assert loading.index('scanDirectory.close()') < loading.index('directoryLoading = false') < loading.index('returnToParent()')
    onexit = function(s, 'void MyLibraryActivity::onExit()')
    assert onexit.index('scanDirectory.close()') < onexit.index('directoryLoading = false')
    batch = function(s, 'void MyLibraryActivity::scanDirectoryBatch()')
    assert 'scanDirectory.close()' not in batch
    assert 'scanDirectory.close()' in function(s, 'void MyLibraryActivity::startDirectoryPage')
    assert 'scanDirectory.close()' in function(s, 'void MyLibraryActivity::finishDirectoryPage')


def directory_moves():
    s = (SRC/'activities/home/MyLibraryActivity.cpp').read_text()
    start = s.index('      bool pasteSuccess = false;')
    end = s.index('      if (pasteSuccess) {', start)
    actual = s[start:end]
    guard = function(s, 'bool canMoveDirectory(')
    code = COMMON + r'''
#include <map>
struct Directory {
 unsigned cluster=0;
 explicit operator bool() const { return cluster!=0; }
 bool isDirectory() const { return cluster!=0; }
 unsigned firstCluster() const { return cluster; }
 void close() {}
};
struct SdShim {
 std::map<std::string, unsigned> dirs{
  {"/", 1}, {"/Books", 2}, {"/books", 2}, {"/BOOKS", 2},
  {"/books/sub", 3}, {"/Books/sub", 3}, {"/Bookshelf", 4}, {"/Other", 5},
  {"/BOOKS~1", 2}, {"/BOOKS~1/sub", 3}, {"/Ä", 6}, {"/ä", 6}, {"/ä/sub", 7}};
 Directory open(const char* path) { return {dirs[path]}; }
 int renames=0;
 bool exists(const char*) { return false; }
 bool rename(const char*, const char*) { ++renames; return true; }
} SdMan;
''' + guard + r'''
bool copyFile(const char*, const char*) { return true; }
bool paste(const std::string& source, const std::string& dstPath) {
 const std::string copySourcePath=source+"/";
 const bool isCutMode=true;
 const std::string basepath=dstPath.substr(0,std::max(size_t(1),dstPath.find_last_of('/')));
''' + actual + r'''
 return pasteSuccess;
}
int main() {
 assert(!paste("/Books", "/Books/sub/Books"));
 assert(!paste("/Books", "/books/sub/Books"));
 assert(!paste("/Books", "/BOOKS/Books"));
 assert(!paste("/Books", "/BOOKS~1/sub/Books"));
 assert(!paste("/Ä", "/ä/sub/Ä"));
 assert(!paste("/Books", "/Unreadable/Books"));
 assert(SdMan.renames==0);
 assert(paste("/Books", "/Bookshelf/Books"));
 assert(paste("/Books", "/Other/Books"));
 assert(paste("/Books", "/MovedBooks"));
 assert(SdMan.renames==3);
}
'''
    run(code, 'actual directory move guard / FAT directory identities, aliases, I/O failure and siblings')

if __name__ == '__main__':
    directory_moves()
    workers()
    library()
