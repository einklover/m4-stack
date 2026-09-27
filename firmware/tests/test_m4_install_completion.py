"""AppInstall worker publication and UI ownership.

The trampoline harness and the activity lifecycle harness are separate
programs. The trampoline's Done::store retires the job; the lifecycle
harness uses a real atomic and must not share that flag.
"""
from test_m4_astra_stability import SRC, COMMON, function, run

s = (SRC / 'activities/apps/AppInstallActivity.cpp').read_text()
header = (SRC / 'activities/apps/AppInstallActivity.h').read_text()


def trampoline():
    actual = function(s, 'void installTaskTrampoline(')
    store_at = actual.index('done.store')
    assert actual.index('job->done.store') < actual.index('vTaskDelete')
    assert 'job->' not in actual[store_at:]
    code = COMMON + r'''
static bool retired=false,deleted=false;
struct TrackedBool{bool value=true;operator bool()const{assert(!retired);return value;}};
struct TrackedString{const char* c_str()const{assert(!retired);return "";}};
struct Result{TrackedBool ok;TrackedString error;};
struct Done {void store(bool,std::memory_order){retired=true;}};
struct AppInstallJob{std::string path;Result result;Done done;};
struct SerialShim{template<class...T>void printf(const char*,T...) {}}Serial;
struct ESPShim{unsigned getFreeHeap(){return 100000;}}ESP;
namespace M4xInstaller{Result install(const std::string&){return {};}}
void vTaskDelete(void*){deleted=true;}
''' + actual + r'''
int main(){AppInstallJob job;installTaskTrampoline(&job);assert(retired&&deleted);}
'''
    run(code, 'actual installer worker / UI reclaims job immediately at done publication')


def ownership():
    do = function(s, 'void AppInstallActivity::doInstall()')
    observe = function(s, 'void AppInstallActivity::observeInstallJob()')
    ready = function(s, 'bool AppInstallActivity::readyForDestruction()')
    sleep = function(s, 'bool AppInstallActivity::preventAutoSleep()')
    dtor = function(s, 'AppInstallActivity::~AppInstallActivity()')
    on_exit = function(s, 'void AppInstallActivity::onExit()')
    loop = function(s, 'void AppInstallActivity::loop()')
    assert 'vTaskDelay' not in do and 'while' not in do and 'xTaskCreate' in do
    assert loop.index('observeInstallJob') < loop.index('M4RenderGuard')
    assert loop.index('M4RenderGuard') < loop.index('if (job_) return')
    assert loop.index('if (job_) return') < loop.index('onDone_')
    for bad in ('vTaskDelete', 'vTaskDelay', 'delete job', 'xSemaphoreTake'):
        assert bad not in on_exit
    assert 'done.load' in ready and 'delete job_' in dtor and 'done.load' in dtor
    assert 'installRunning_' not in header and 'displayTask' not in header
    assert 'job_' in header and 'readyForDestruction' in header
    code = COMMON + r'''
#include <utility>
static int jobLive=0;
static bool createOk=true;
static int createCalls=0;
static unsigned long gMillis=1000;
static bool parentReady=true;
static int baseExits=0;
unsigned long millis(){return gMillis;}
using BaseType_t=int;
using UBaseType_t=unsigned;
constexpr int pdPASS=1;
void installTaskTrampoline(void*) {}
BaseType_t xTaskCreate(void (*)(void*), const char*, unsigned, void*, UBaseType_t, void**){
  ++createCalls;
  return createOk ? pdPASS : 0;
}
struct Manifest{std::string name,version;int versionCode=0;};
struct M4xInstallResult{bool ok=false;std::string message,error;Manifest manifest;};
struct AppInstallJob{
  AppInstallJob(){++jobLive;}
  ~AppInstallJob(){--jobLive;}
  std::string path;
  M4xInstallResult result;
  std::atomic<bool> done{false};
};
enum class Stage { Pick, Confirm, Result };
class ActivityWithSubactivity {
 public:
  void onExit(){++baseExits;}
  bool readyForDestruction() const {return parentReady;}
};
class AppInstallActivity : public ActivityWithSubactivity {
 public:
  ~AppInstallActivity();
  void onExit();
  bool readyForDestruction() const;
  bool preventAutoSleep();
  void doInstall();
  void observeInstallJob();
  std::string packagePath_;
  Stage stage_=Stage::Pick;
  std::string resultMessage_;
  bool updateRequired_=false;
  AppInstallJob* job_=nullptr;
  unsigned long installStartedMs_=0;
  bool installTimedOut_=false;
  M4xInstallResult probe_{};
};
''' + do + observe + ready + sleep + dtor + on_exit + r'''
int main(){
  createOk=false;
  auto* failed=new AppInstallActivity();
  failed->packagePath_="/apps_inbox/a.m4x";
  failed->doInstall();
  assert(failed->job_==nullptr);
  assert(failed->resultMessage_.find("内存不足")!=std::string::npos);
  assert(jobLive==0);
  delete failed;

  createOk=true;
  createCalls=0;
  gMillis=5000;
  auto* a=new AppInstallActivity();
  a->packagePath_="/apps_inbox/b.m4x";
  a->doInstall();
  assert(createCalls==1);
  assert(a->job_!=nullptr);
  assert(a->job_->path=="/apps_inbox/b.m4x");
  assert(!a->job_->done.load());
  assert(jobLive==1);
  assert(a->preventAutoSleep());
  assert(!a->readyForDestruction());
  int calls=createCalls;
  a->doInstall();
  assert(createCalls==calls);

  gMillis=a->installStartedMs_+60001;
  a->observeInstallJob();
  assert(a->job_!=nullptr);
  assert(a->installTimedOut_);
  assert(a->resultMessage_=="安装超时");
  assert(!a->probe_.ok);
  assert(!a->readyForDestruction());
  assert(jobLive==1);
  a->observeInstallJob();
  assert(a->job_!=nullptr&&jobLive==1);

  a->onExit();
  assert(baseExits==1);
  assert(a->job_!=nullptr);
  auto* pending=a->job_;
  delete a;
  assert(jobLive==1);
  assert(!pending->done.load());
  delete pending;
  assert(jobLive==0);

  gMillis=8000;
  auto* done=new AppInstallActivity();
  done->doInstall();
  done->job_->result.ok=false;
  done->job_->result.message="安装包不存在";
  done->installTimedOut_=true;
  done->resultMessage_="安装超时";
  done->job_->done.store(true, std::memory_order_release);
  done->observeInstallJob();
  assert(done->job_==nullptr);
  assert(jobLive==0);
  assert(!done->installTimedOut_);
  assert(done->resultMessage_=="安装包不存在");
  assert(!done->probe_.ok);
  assert(!done->preventAutoSleep());
  assert(done->readyForDestruction());
  delete done;

  auto* ok=new AppInstallActivity();
  ok->doInstall();
  ok->job_->result.ok=true;
  ok->job_->result.manifest.name="钟";
  ok->job_->result.manifest.version="2";
  ok->job_->result.manifest.versionCode=2;
  ok->job_->done.store(true, std::memory_order_release);
  ok->observeInstallJob();
  assert(ok->job_==nullptr);
  assert(ok->resultMessage_.find("安装成功")!=std::string::npos);
  assert(ok->resultMessage_.find("钟")!=std::string::npos);
  assert(ok->readyForDestruction());
  delete ok;

  auto* held=new AppInstallActivity();
  held->doInstall();
  held->job_->done.store(true, std::memory_order_release);
  delete held;
  assert(jobLive==0);

  parentReady=false;
  AppInstallActivity idle;
  assert(!idle.readyForDestruction());
  parentReady=true;
  assert(idle.readyForDestruction());
}
'''
    run(code, 'actual install ownership / timeout keeps the job until done')


if __name__ == '__main__':
    trampoline()
    ownership()
