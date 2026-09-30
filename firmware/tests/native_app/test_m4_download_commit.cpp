#include "../../src/util/M4DownloadCommit.h"
#include <cassert>
#include <map>
#include <string>
struct Files {
 std::map<std::string,std::string> data{{"file","old"},{"tmp","new"}};
 int step=0, fail=0;bool failRollback=false;
 bool exists(const char* p){return data.count(p);}
 bool rename(const char* a,const char* b){if(++step==fail || (failRollback && std::string(a)=="bak"))return false;if(!exists(a)||exists(b))return false;data[b]=data[a];data.erase(a);return true;}
 bool remove(const char* p){if(++step==fail)return false;return data.erase(p);}
};
int main(){
 for(int fault=0;fault<=3;++fault){Files f;f.fail=fault;bool ok=M4DownloadCommit::commit(f,"tmp","file","bak");assert(f.data["file"]=="old"||f.data["file"]=="new"||f.data["bak"]=="old");if(ok)assert(f.data["file"]=="new");}
 Files f;f.data["bak"]="recovery";assert(!M4DownloadCommit::commit(f,"tmp","file","bak"));assert(f.data["bak"]=="recovery"&&f.data["file"]=="old");
 Files rollback;rollback.fail=2;rollback.failRollback=true;assert(!M4DownloadCommit::commit(rollback,"tmp","file","bak"));assert(rollback.data["bak"]=="old"&&rollback.data["tmp"]=="new");
 Files fresh;fresh.data.erase("file");assert(M4DownloadCommit::commit(fresh,"tmp","file","bak"));assert(fresh.data["file"]=="new");
}
