#include <cassert>
#include <map>
#include <string>
#include "../../src/util/M4SettingsCommit.h"

struct FakeFiles {
  std::map<std::string, std::string> data;
  int failAt = -1;
  int writes = 0;
  bool exists(const char* p) { return data.count(p) != 0; }
  bool remove(const char* p) {
    if (++writes == failAt) return false;
    return data.erase(p) != 0;
  }
  bool rename(const char* from, const char* to) {
    if (++writes == failAt) return false;
    auto it = data.find(from);
    if (it == data.end() || exists(to)) return false;
    data[to] = it->second;
    data.erase(it);
    return true;
  }
};

int main() {
  for (bool invalid : {false, true}) {
    for (int fail = -1; fail <= 5; ++fail) {
      FakeFiles fs{{{"tmp", "NEW"}, {"primary", invalid ? "CORRUPT" : "OLD"}, {"backup", "OLDER"}}, fail};
      bool flag = invalid;
      const bool ok = M4SettingsCommit::commit(fs, "tmp", "primary", "backup", flag);
      auto valid = [](const std::string& value) {
        return value == "NEW" || value == "OLD" || value == "OLDER";
      };
      assert((fs.exists("primary") && valid(fs.data["primary"])) ||
             (fs.exists("backup") && valid(fs.data["backup"])));
      if (ok) assert(fs.data["primary"] == "NEW" && !flag);
    }
  }
}
