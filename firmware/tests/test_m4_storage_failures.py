"""Execute real cache-clear and TXT commit bodies on an isolated in-memory FS."""
from test_m4_astra_stability import SRC, COMMON, function, run

SHIM = COMMON + r'''
#include <map>
struct String : std::string {
 using std::string::string; String(const std::string& s):std::string(s){}
 bool startsWith(const char* p) const{return rfind(p,0)==0;}
};
uint32_t millis(){return 0;}
struct SerialShim {template<class... A>void printf(const char*,A...) {}} Serial;
struct Node {bool dir=false;std::string data;};
static std::map<std::string,Node> disk;
static bool syncOk=true,closeOk=true,shortWrite=false,removeOk=true,renameOk=true,busy=false;
static std::string nameError,readError;
static std::vector<std::string> removed;
struct FsFile {
 std::string path;bool valid=false;int error=0;size_t at=0;std::vector<std::string> children;
 FsFile()=default;
 FsFile(std::string p):path(p),valid(disk.count(p)) {
  if(valid&&disk[p].dir)for(auto& entry:disk)if(entry.first.rfind(p+"/",0)==0&&entry.first.find('/',p.size()+1)==std::string::npos)children.push_back(entry.first);
 }
 explicit operator bool()const{return valid;}
 bool isDirectory()const{return valid&&disk.at(path).dir;}
 FsFile openNextFile(){if(path==readError){error=1;return {}; }while(at<children.size()){auto p=children[at++];if(disk.count(p))return FsFile(p);}return {};}
 size_t getName(char* p,size_t n){auto name=path.substr(path.find_last_of('/')+1);if(path==nameError||name.size()>=n){p[0]=0;return 0;}memcpy(p,name.c_str(),name.size()+1);return name.size();}
 int getError()const{return error;}
 int getWriteError()const{return 0;}
 size_t write(const uint8_t* p,size_t n){size_t out=shortWrite?n/2:n;disk[path].data.append(reinterpret_cast<const char*>(p),out);return out;}
 bool sync(){return syncOk;}
 bool close(){valid=false;return closeOk;}
};
struct SdShim {
 bool exists(const char* p){return disk.count(p);}
 FsFile open(const char* p){return FsFile(p);}
 bool openFileForWrite(const char*,const char* p,FsFile& f){if(disk.count(p)&&disk[p].dir)return false;disk[p]={false,{}};f=FsFile(p);return true;}
 bool remove(const char* p){if(!removeOk||!disk.count(p)||disk[p].dir)return false;removed.push_back(p);disk.erase(p);return true;}
 bool removeDir(const char* p){if(!removeOk)return false;std::string prefix=std::string(p)+"/";for(auto i=disk.begin();i!=disk.end();){if(i->first==p||i->first.rfind(prefix,0)==0){removed.push_back(i->first);i=disk.erase(i);}else ++i;}return true;}
 bool rename(const char* a,const char* b){if(!renameOk||!disk.count(a)||disk.count(b)||disk[a].dir)return false;disk[b]=disk[a];disk.erase(a);return true;}
} SdMan;
bool m4ReaderCacheBusy(){return busy;}
void reset(){disk.clear();removed.clear();syncOk=closeOk=removeOk=renameOk=true;shortWrite=busy=false;nameError=readError="";}
'''

def cache():
 s=(SRC/'activities/settings/ClearCacheActivity.cpp').read_text()
 code=SHIM+'\n#include "'+str(SRC/'util/M4CacheClearPolicy.h')+'"\n'+r'''
struct ClearCacheActivity {enum {WARNING,CLEARING,SUCCESS,FAILED};int state=WARNING,clearedCount=0,failedCount=0;bool updateRequired=false;void clearCache();};
'''+function(s,'void ClearCacheActivity::clearCache()')+r'''
void seed(){reset();disk["/.crosspoint"]={true,{}};disk["/.crosspoint/txt_book"]={true,{}};
 for(auto n:{"progress.dat","PROGRESS.BIN","progress.TMP","index.bin"})disk[std::string("/.crosspoint/txt_book/")+n]={false,n};
 disk["/.crosspoint/txt_book/pages"]={true,{}};disk["/.crosspoint/txt_book/pages/1"]={false,"cache"};
 disk["/.crosspoint/settings.json"]={false,"settings"};}
void protectedFiles(){assert(disk.count("/.crosspoint/txt_book"));assert(disk["/.crosspoint/txt_book/progress.dat"].data=="progress.dat");assert(disk["/.crosspoint/txt_book/PROGRESS.BIN"].data=="PROGRESS.BIN");assert(disk["/.crosspoint/txt_book/progress.TMP"].data=="progress.TMP");assert(disk["/.crosspoint/settings.json"].data=="settings");}
int main(){
 seed();ClearCacheActivity ok;ok.clearCache();assert(ok.state==ok.SUCCESS);protectedFiles();assert(!disk.count("/.crosspoint/txt_book/index.bin")&&!disk.count("/.crosspoint/txt_book/pages"));
 for(int fault=0;fault<5;++fault){seed();if(fault==0)readError="/.crosspoint";if(fault==1)readError="/.crosspoint/txt_book";if(fault==2)nameError="/.crosspoint/txt_book/index.bin";if(fault==3)removeOk=false;if(fault==4)busy=true;ClearCacheActivity a;a.clearCache();assert(a.state==a.FAILED);protectedFiles();}
}
'''
 run(code,'actual Clear Cache / progress case aliases, busy owners, name/read/delete errors')

def progress():
 s=function((SRC/'activities/reader/TxtReaderActivity.cpp').read_text(),'void TxtReaderActivity::persistProgressSnapshot')
 actual=s[s.index('  snapshot.owner->setupCacheDir();'):s.rfind('}')]
 assert 'forceRemove' not in s and 'removeDir' not in s
 code=SHIM+r'''
struct Owner{void setupCacheDir(){}};
struct Snapshot {Owner* owner;std::string dir="/book";uint32_t generation=1;std::vector<uint8_t> data={'N','E','W'};int chapter=1,page=1,totalPages=1;};
std::atomic<uint32_t> progressGeneration_{1};
void persist(const Snapshot& snapshot){
'''+actual+r'''
}
int main(){
 Owner owner;Snapshot s{&owner};
 for(int fault=0;fault<6;++fault){reset();disk["/book/progress.dat"]={false,"OLD"};disk["/book/progress.bin"]={false,"LEGACY"};
 if(fault==1)shortWrite=true;if(fault==2)syncOk=false;if(fault==3)closeOk=false;if(fault==4)removeOk=false;if(fault==5)renameOk=false;
 persist(s);assert(disk["/book/progress.dat"].data=="OLD"||disk["/book/progress.tmp"].data=="NEW"||disk["/book/progress.dat"].data=="NEW");assert(disk["/book/progress.bin"].data=="LEGACY");
 if(fault==0)assert(disk["/book/progress.dat"].data=="NEW");}
 reset();disk["/book/progress.tmp"]={false,"ONLY"};renameOk=false;persist(s);assert(disk["/book/progress.tmp"].data=="ONLY");
 reset();disk["/book/progress.tmp"]={false,"ONLY"};syncOk=false;persist(s);assert(disk["/book/progress.dat"].data=="ONLY");
 reset();disk["/book/progress.dat"]={false,"OLD"};disk["/book/progress.tmp"]={true,{}};disk["/book/progress.tmp/user.txt"]={false,"USER"};persist(s);assert(disk["/book/progress.tmp/user.txt"].data=="USER"&&disk["/book/progress.dat"].data=="OLD");
}
'''
 run(code,'actual TXT progress transaction / short write, sync, close, remove, rename, sole temp, directory')

if __name__=='__main__':
 cache();progress()
