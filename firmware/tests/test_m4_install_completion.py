from test_m4_astra_stability import SRC,COMMON,function,run
s=(SRC/'activities/apps/AppInstallActivity.cpp').read_text()
actual=function(s,'void installTaskTrampoline(')
code=COMMON+r'''
static bool retired=false,deleted=false;
struct TrackedBool{bool value=true;operator bool()const{assert(!retired);return value;}};
struct TrackedString{const char* c_str()const{assert(!retired);return "";}};
struct Result{TrackedBool ok;TrackedString error;};
struct Done {void store(bool,std::memory_order){retired=true;}};
struct InstallJob{std::string path;Result result;Done done;};
struct SerialShim{template<class...T>void printf(const char*,T...) {}}Serial;
struct ESPShim{unsigned getFreeHeap(){return 100000;}}ESP;
namespace M4xInstaller{Result install(const std::string&){return {};}}
void vTaskDelete(void*){deleted=true;}
'''+actual+r'''
int main(){InstallJob job;installTaskTrampoline(&job);assert(retired&&deleted);}
'''
if __name__=='__main__':run(code,'actual installer worker / UI reclaims job immediately at done publication')
