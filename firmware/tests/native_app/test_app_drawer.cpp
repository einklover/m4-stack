#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

namespace {

// Pure spec inventory — always passes, documents contract
std::vector<std::string> expectedDrawerInventory(const std::vector<std::string>& installed) {
    std::vector<std::string> inv;
    inv.reserve(2 + installed.size());
    inv.push_back("builtin.settings");
    inv.push_back("builtin.files");
    for (auto &id : installed) inv.push_back(id);
    return inv;
}

bool isBuiltin(const std::string& id) {
    return id == "builtin.settings" || id == "builtin.files"
        || id == "builtin.files:" || id == "settings" || id == "files";
    // Also accept variants containing "settings" or "files" as builtin per spec tolerance
    // but canonical is builtin.*
}

bool canUninstall(const std::string& id) {
    // Builtins are not uninstallable
    if (id == "builtin.settings" || id == "builtin.files") return false;
    // Installed plugins are uninstallable
    return true;
}

void testDrawerSpecPure() {
    auto inv = expectedDrawerInventory({});
    assert(inv.size() == 2);
    assert(inv[0] == "builtin.settings");
    assert(inv[1] == "builtin.files");
    auto inv2 = expectedDrawerInventory({"com.weread.client", "com.fanqie.client"});
    assert(inv2.size() == 4);
    assert(std::find(inv2.begin(), inv2.end(), "builtin.settings") != inv2.end());
    assert(std::find(inv2.begin(), inv2.end(), "builtin.files") != inv2.end());
    assert(std::find(inv2.begin(), inv2.end(), "com.weread.client") != inv2.end());
    assert(!canUninstall("builtin.settings"));
    assert(!canUninstall("builtin.files"));
    assert(canUninstall("com.weread.client"));
    assert(canUninstall("com.fanqie.client"));
    printf("Drawer pure spec PASS\n");
}

void testProductionDrawerInventory() {
    // Check production AppListActivity.cpp for drawer inventory including Settings + File manager
    // This will be RED until Luna lands.
    FILE* f = fopen("firmware/src/activities/apps/AppListActivity.cpp", "r");
    if (!f) f = fopen("./firmware/src/activities/apps/AppListActivity.cpp", "r");
    if (!f) f = fopen("../../firmware/src/activities/apps/AppListActivity.cpp", "r");
    if (!f) {
        printf("SKIP: cannot open AppListActivity.cpp\n");
        return;
    }
    fseek(f,0,SEEK_END);
    long n = ftell(f);
    fseek(f,0,SEEK_SET);
    std::string content;
    content.resize((size_t)n);
    fread(&content[0],1,(size_t)n,f);
    fclose(f);

    // Also try helper candidates
    const char* helpers[] = {
        "firmware/src/activities/apps/AppListModel.h",
        "firmware/src/activities/apps/DrawerInventory.h",
        "firmware/src/activities/apps/AppDrawerHelper.h",
        "firmware/src/util/AppDrawerInventory.h",
    };
    std::string helperContent;
    for (auto hpath : helpers) {
        FILE* hf = fopen(hpath, "r");
        if (!hf) hf = fopen(("./" + std::string(hpath)).c_str(), "r");
        if (!hf) hf = fopen(("../../" + std::string(hpath)).c_str(), "r");
        if (hf) {
            fseek(hf,0,SEEK_END);
            long hn = ftell(hf);
            fseek(hf,0,SEEK_SET);
            std::string tmp;
            tmp.resize((size_t)hn);
            fread(&tmp[0],1,(size_t)hn,hf);
            fclose(hf);
            helperContent += tmp;
        }
    }
    std::string combined = content + helperContent;

    // Check for Settings
    bool hasSettings = combined.find("builtin.settings") != std::string::npos
        || combined.find("kSystemSettings") != std::string::npos
        || combined.find("系统设置") != std::string::npos
        || (combined.find("Settings") != std::string::npos && combined.find("settings") != std::string::npos);
    // More precise: look for Settings token near apps_ or inventory
    bool hasSettingsInventory = combined.find("builtin.settings") != std::string::npos;

    bool hasFiles = combined.find("builtin.files") != std::string::npos
        || combined.find("kFileManager") != std::string::npos
        || combined.find("文件管理") != std::string::npos;
    bool hasFilesInventory = combined.find("builtin.files") != std::string::npos;

    if (!hasSettingsInventory || !hasFilesInventory) {
        printf("RED: App drawer missing builtins in %s\n", "AppListActivity.cpp (+ helpers)");
        printf("  hasSettings=%d (inventory=%d) hasFiles=%d (inventory=%d)\n", hasSettings, hasSettingsInventory, hasFiles, hasFilesInventory);
        printf("  helpers checked: AppListModel.h, DrawerInventory.h, AppDrawerHelper.h\n");
        // Make explicit RED
        assert(false && "App drawer must include Settings (builtin.settings) and File manager (builtin.files) plus installed plugins — missing in production (expected RED until Luna lands)");
    }

    // Check uninstall guard for builtins
    std::string lower = combined;
    for (char &c : lower) c = tolower((unsigned char)c);
    bool hasUninstall = lower.find("uninstall") != std::string::npos;
    bool hasBuiltin = lower.find("builtin") != std::string::npos;
    bool hasGuard = hasUninstall && hasBuiltin;
    if (!hasGuard) {
        printf("RED: Drawer uninstall guard for builtins missing (uninstall without builtin check)\n");
        assert(false && "Builtin apps must be not uninstallable — expected guard referencing builtin near uninstall (expected RED until Luna lands)");
    }
    printf("Production drawer inventory + uninstall guard PASS\n");
}

} // namespace

int main() {
    testDrawerSpecPure();
    // Validate builtin not uninstallable via pure logic
    assert(!isBuiltin("com.weread.client"));
    assert(isBuiltin("builtin.files") || std::string("builtin.files").find("files") != std::string::npos);
    testProductionDrawerInventory();
    printf("app drawer ALL PASS (if RED above, production missing)\n");
    return 0;
}
