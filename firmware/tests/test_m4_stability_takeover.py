"""Execute changed production method bodies against deterministic host faults."""
from pathlib import Path
from test_m4_astra_stability import function, run, COMMON
ROOT = Path(__file__).resolve().parents[2]

def idle_flush():
    text = (ROOT/'firmware/lib/EpdFont/TtfEpdFont.cpp').read_text()
    code = COMMON + r'''
#include <new>
#define ESP32 1
#define M4_SD_GLYPH_CACHE_ENABLED 1
using SemaphoreHandle_t = int*;
using TickType_t = unsigned;
constexpr int pdTRUE=1, portMAX_DELAY=10000;
static int gives=0; static bool busy=false;
int xSemaphoreTake(int* p, unsigned wait) { assert(wait==0); if(busy) return 0; assert(!*p); *p=1; return 1; }
void xSemaphoreGive(int* p) { assert(*p==1); *p=0; ++gives; }
unsigned millis(){return 123;}
'''+function(text,'class FaceLock {')+r''';
struct TtfEpdFont {
 bool valid_=true, dirty=true, fail=true;
 int mu=0; int* mutex_=&mu; unsigned lastTransientSkipMs_=0;
 bool flushBackedOff() const { return false; }
 int flushDirtySlots(int) { assert(mu==1); if(fail) throw std::bad_alloc(); dirty=false; return 1; }
 static int idleFlushDirty(int);
};
constexpr int kLiveMax=1; TtfEpdFont* gLive[1];
'''+function(text,'int TtfEpdFont::idleFlushDirty(')+r'''
int main(){TtfEpdFont f;gLive[0]=&f;
assert(TtfEpdFont::idleFlushDirty(1)==0);assert(f.dirty && f.mu==0 && gives==1 && f.lastTransientSkipMs_==123);
busy=true;assert(TtfEpdFont::idleFlushDirty(1)==0);assert(gives==1);
busy=false;f.fail=false;assert(TtfEpdFont::idleFlushDirty(1)==1);assert(!f.dirty && f.mu==0 && gives==2);}
'''
    run(code,'actual idle flush / OOM release, retry, contended lock')

def display_exit():
    text=(ROOT/'firmware/src/activities/network/CrossPointWebServerActivity.cpp').read_text()
    header=(ROOT/'firmware/src/activities/network/CrossPointWebServerActivity.h').read_text()
    assert 'displayTaskHandle' not in text + header
    assert 'renderingMutex' not in text + header
    assert 'renderPendingUpdate();' in function(text, 'void CrossPointWebServerActivity::loop()')
    code=COMMON+r'''
#include <new>
struct SerialType{template<class...A>void printf(const char*,A...) {}}Serial;
struct CrossPointWebServerActivity {
 bool updateRequired=true,fail=true; int renders=0;
 void render(){++renders;if(fail)throw std::bad_alloc();}
 void renderPendingUpdate();
};
'''+function(text,'void CrossPointWebServerActivity::renderPendingUpdate()')+r'''
int main(){CrossPointWebServerActivity a;a.renderPendingUpdate();assert(a.renders==1 && !a.updateRequired);
a.renderPendingUpdate();assert(a.renders==1);a.fail=false;a.updateRequired=true;a.renderPendingUpdate();assert(a.renders==2);}
'''
    run(code,'actual owner-loop web rendering / OOM, no orphan display task or mutex')

def inbox_scan():
    text=(ROOT/'firmware/open-m4-sdk/libs/hardware/SDCardManager/src/SDCardManager.cpp').read_text()
    code=COMMON+r'''
#include <new>
using String=std::string; static unsigned now=0,delayMs=0;static int opens=0,closed=0,total=0;static bool directories=true;
unsigned millis(){return now;}
struct FsFile{
 bool root=true,valid=true; int index=0;
 explicit operator bool()const{return valid;}
 bool isDirectory()const{return root||directories;}
 void getName(char* p,size_t n){snprintf(p,n,"f%d.m4x",index);}
 void close(){if(valid&&!root)++closed;valid=false;}
 FsFile openNextFile(){++opens;now+=delayMs;if(index>=total)return {false,false,index};return {false,true,index++};}
};
struct Volume{FsFile open(const char*){return {};}};
struct SDCardManager{bool initialized=true;Volume volume;Volume& vol(){return volume;}std::vector<String> listFiles(const char*,int,bool*);};
struct SerialType{explicit operator bool()const{return true;}template<class...A>void printf(const char*,A...){};}Serial;
'''+function(text,'std::vector<String> SDCardManager::listFiles(')+r'''
int main(){SDCardManager sd;bool partial=true;
assert(sd.listFiles("/",0,&partial).empty() && !partial && opens==0);
total=10000;assert(sd.listFiles("/",64,&partial).empty());assert(partial&&opens==512&&closed==512);
opens=closed=now=0;delayMs=100;assert(sd.listFiles("/",64,&partial).empty());assert(partial&&opens==4&&closed==4);
opens=closed=now=0;delayMs=0;directories=false;total=3;assert(sd.listFiles("/",64,&partial).size()==3);assert(!partial&&opens==4&&closed==3);
opens=closed=0;total=100;assert(sd.listFiles("/",64,&partial).size()==64);assert(partial&&opens==64&&closed==64);}
'''
    run(code,'actual inbox enumeration / directory-heavy, slow IO, partial and child closure')

if __name__=='__main__':
    idle_flush()
    display_exit()
    inbox_scan()
