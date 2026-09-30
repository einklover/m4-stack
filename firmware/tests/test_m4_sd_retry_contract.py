from test_m4_astra_stability import SRC, ROOT, COMMON, function, run
from test_m4_storage_failures import SHIM

def retry():
 main=(SRC/'main.cpp').read_text()
 wait=function(main,'static void waitForSdRetryInput()')
 code=COMMON+r'''
static unsigned step=0;static bool tapMode=false;
void delay(unsigned) {assert(step<100);}
namespace MappedInputManager{enum class Button{Confirm};}
struct Gpio{void update(){++step;}bool wasTouchTap(float&,float&){return tapMode && step==4;}}gpio;
struct Input{bool isPressed(MappedInputManager::Button){return !tapMode && (step<3 || (step>=5&&step<8));}}mappedInputManager;
'''+wait+r'''
int main(){waitForSdRetryInput();assert(step==8);step=0;tapMode=true;waitForSdRetryInput();assert(step==4);}
'''
 run(code,'actual SD retry input / held boot key, fresh press, touch')
 probe_block=main[main.index('while (!SdMan.capabilityProbe'):main.index('    m4SdOk = true;',main.index('while (!SdMan.capabilityProbe'))]
 assert 'SdMan.begin()' not in probe_block and 'waitForSdRetryInput();' in probe_block
 sdk=(ROOT/'firmware/open-m4-sdk/libs/hardware/SDCardManager/src/SDCardManager.cpp').read_text()
 for body in sdk.split('bool SDCardManager::begin() {')[1:]:
  assert body.index('if (initialized) return true;') < body.index('_powerHook') if '_powerHook' in body else True
 body=function(sdk,'bool SDCardManager::capabilityProbe(')
 shim=SHIM.replace(' int getError()const', ' bool openNext(FsFile* root,int){auto next=root->openNextFile();*this=next;return valid;}\n int read(uint8_t*,size_t n){return n;}\n bool seekSet(int){return true;}\n int getError()const')
 code=shim+r'''
constexpr int O_RDONLY=0;
struct SDCardManager {bool initialized=false;std::string code;SdShim& vol(){return SdMan;}void setLast(const char*,const char*,const char* c){code=c;}bool openFileForRead(const char*,const char* p,FsFile& f){f=SdMan.open(p);return bool(f);}bool capabilityProbe(const char*);};
'''+body+r'''
int main(){SDCardManager m;assert(!m.capabilityProbe(nullptr));m.initialized=true;disk["/"]={true,{}};readError="/";assert(!m.capabilityProbe(nullptr));assert(m.code=="io_failure");readError="";assert(m.capabilityProbe(nullptr));assert(m.code=="ok");}
'''
 run(code,'actual SD capability probe / root I/O failure then read-only recovery')

if __name__=='__main__':retry()
