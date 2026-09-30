"""Exercise the production download body and sink with decoded transport chunks.
Chunk framing itself remains Arduino HTTPClient's responsibility; this host test
proves bounded writes and commit/rollback, not TLS or the real HTTP parser.
"""
from test_m4_astra_stability import SRC, function, run
from test_m4_storage_failures import SHIM

s=(SRC/'network/HttpDownloader.cpp').read_text()
sink=s[s.index('class BoundedWriteStream'):s.index('HttpDownloader::DownloadError downloadBody')]
body=function(s,'HttpDownloader::DownloadError downloadBody')
preamble=r'''
#include <cstdint>
#include <cstddef>
#include <functional>
struct Stream {
 virtual size_t write(uint8_t){return 0;}
 virtual size_t write(const uint8_t*,size_t){return 0;}
 virtual int available(){return 0;} virtual int read(){return -1;} virtual int peek(){return -1;} virtual void flush(){}
};
struct WiFiClient { bool stopped=false;void stop(){stopped=true;} };
struct HttpDownloader {using ProgressCallback=std::function<void(size_t,size_t)>;enum DownloadError{OK,HTTP_ERROR,FILE_ERROR,ABORTED};};
void delay(int){}
constexpr int O_WRONLY=1,O_CREAT=2,O_EXCL=4;
'''
shim=SHIM.replace('struct FsFile {','struct FsFile : Stream {').replace(' FsFile open(const char* p){return FsFile(p);}', ''' FsFile open(const char* p,int flags=0){if(flags){if(disk.count(p))return {};disk[p]={false,{}};}return FsFile(p);}''')
code=preamble+shim+'\n#include "'+str(SRC/'network/M4HttpDownloadPolicy.h')+'"\n#include "'+str(SRC/'util/M4DownloadCommit.h')+'"\n'+r'''
namespace UrlUtils {bool isHttpsUrl(const std::string&){return false;}}
static bool expiredMode=false;
void startBody(WiFiClient&,bool,uint32_t){}
bool bodyExpired(WiFiClient&,bool){return expiredMode;}
struct HTTPClient {
 int length=-1;std::string decoded="abcdefghijkl";bool fail=false;
 int getSize(){return length;}
 int writeToStream(Stream* sink){if(fail)return -1;int total=0;for(size_t i=0;i<decoded.size();i+=2){size_t n=std::min(size_t(2),decoded.size()-i);auto written=sink->write(reinterpret_cast<const uint8_t*>(decoded.data()+i),n);total+=written;if(written!=n)return -1;}return total;}
 void end(){}
};
'''+sink+body+r'''
int main(){
 for(int fault=0;fault<9;++fault){reset();expiredMode=false;disk["book"]={false,"OLD"};HTTPClient http;WiFiClient client;
 if(fault==1)http.fail=true;if(fault==2)shortWrite=true;if(fault==3)syncOk=false;if(fault==4)closeOk=false;if(fault==5)http.length=50;
 if(fault==6){expiredMode=true;http.decoded.clear();}if(fault==7)renameOk=false;
 size_t limit=fault==8?5:128;
 auto result=downloadBody(http,client,"http://test.invalid","book",limit,nullptr);
 if(fault==0){assert(result==HttpDownloader::OK);assert(disk["book"].data==http.decoded);}else{assert(result!=HttpDownloader::OK);assert(disk["book"].data=="OLD");}
 }
 reset();expiredMode=false;disk["book"]={false,"OLD"};disk["book.m4-download.tmp"]={false,"RECOVERY"};HTTPClient h;WiFiClient c;
 assert(downloadBody(h,c,"http://test.invalid","book",128,nullptr)==HttpDownloader::FILE_ERROR);assert(disk["book.m4-download.tmp"].data=="RECOVERY"&&disk["book"].data=="OLD");
 reset();disk["book"]={false,"OLD"};disk["book.m4-download.bak"]={false,"BACKUP"};assert(downloadBody(h,c,"http://test.invalid","book",128,nullptr)==HttpDownloader::FILE_ERROR);assert(disk["book.m4-download.bak"].data=="BACKUP");
}
'''
if __name__=='__main__':
 run(code,'actual HTTP sink+download transaction / bounded chunks and 8 failure modes')
