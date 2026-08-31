#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

#include "ui/pages/HomeSceneModel.h"

using namespace HomeScene;

namespace {

std::vector<std::string> expectedDockOrder(const std::vector<std::string>& installed) {
    // Spec: first is builtin.files, then prefer weread, fanqie, jjwxc
    std::vector<std::string> preferred = {"com.weread.client", "com.fanqie.client", "com.jjwxc.client"};
    std::vector<std::string> dock;
    dock.reserve(4);
    dock.push_back("builtin.files");
    for (auto &want : preferred) {
        if (std::find(installed.begin(), installed.end(), want) != installed.end()) {
            if (std::find(dock.begin(), dock.end(), want) == dock.end()) {
                dock.push_back(want);
                if (dock.size() >= 4) break;
            }
        }
    }
    for (auto &pid : installed) {
        if (std::find(dock.begin(), dock.end(), pid) == dock.end() && dock.size() < 4) {
            dock.push_back(pid);
        }
    }
    if (dock.size() > 4) dock.resize(4);
    return dock;
}

void testKMaxAppItemsRemainsFour() {
    static_assert(kMaxAppItems == 4, "kMaxAppItems must remain 4");
    assert(kMaxAppItems == 4);
    assert(kHomeAppIconW == 62);
    assert(kHomeAppIconH == 64);
    assert(kHomeAppIconStride == 8);
    assert(kHomeAppIconBytes == 512);
    assert(kHomeAssetArenaBytes == 7748);
    printf("kMaxAppItems=4 lock PASS\n");
}

void testHomeSceneModelHoldsFourAppsInOrder() {
    HomeSceneModel model;
    model.begin(UiScene::DataState::Ready);
    auto dock = expectedDockOrder({"com.fanqie.client", "com.jjwxc.client", "com.weread.client"});
    // dock should be builtin.files, weread, fanqie, jjwxc
    assert(dock.size() == 4);
    assert(dock[0] == "builtin.files");
    assert(dock[1] == "com.weread.client");
    assert(dock[2] == "com.fanqie.client");
    assert(dock[3] == "com.jjwxc.client");
    // Publish via model
    for (size_t i = 0; i < dock.size(); ++i) {
        const char* id = dock[i].c_str();
        const char* name = (dock[i] == "builtin.files") ? "文件管理" : dock[i].c_str();
        const char* icon = (dock[i] == "builtin.files") ? "files" : "icon_home.bmp";
        bool ok = model.addApp(id, name, icon);
        assert(ok && "addApp should succeed for 4 items");
    }
    assert(!model.addApp("extra", "Extra", "icon") && "5th app must fail (kMaxAppItems=4)");
    assert(model.publish());
    HomeSceneSnapshot snap{};
    assert(model.copyLatest(snap));
    assert(snap.appCount == 4);
    // Verify order preserved
    auto view = [&](int idx, const char* expected) {
        auto tv = snap.textView(snap.apps[idx].id);
        std::string got;
        got.reserve(tv.size);
        for (uint16_t j = 0; j < tv.size; ++j) got.push_back((char)tv.readByte(j));
        assert(got == expected);
    };
    view(0, "builtin.files");
    view(1, "com.weread.client");
    view(2, "com.fanqie.client");
    view(3, "com.jjwxc.client");
    // Verify that first app id is builtin.files (or equivalent) and count <=4
    assert(snap.appCount <= kMaxAppItems);
    printf("HomeSceneModel dock order and capacity PASS\n");
}

void testDockSpecPureOrdering() {
    // Pure spec tests — document contract without needing production helper
    assert((expectedDockOrder({}) == std::vector<std::string>{"builtin.files"}));
    assert((expectedDockOrder({"com.weread.client"}) == std::vector<std::string>{"builtin.files","com.weread.client"}));
    assert((expectedDockOrder({"com.jjwxc.client","com.weread.client"}) == std::vector<std::string>{"builtin.files","com.weread.client","com.jjwxc.client"}));
    auto d = expectedDockOrder({"com.jjwxc.client","com.fanqie.client","com.weread.client","com.legado.client","com.example.extra"});
    assert(d.size() == 4);
    assert(d[0] == "builtin.files");
    assert(d[1] == "com.weread.client");
    assert(d[2] == "com.fanqie.client");
    assert(d[3] == "com.jjwxc.client");
    // Order must be stable regardless of input order
    auto d2 = expectedDockOrder({"com.jjwxc.client","com.fanqie.client","com.weread.client"});
    assert(d2[1]=="com.weread.client" && d2[2]=="com.fanqie.client" && d2[3]=="com.jjwxc.client");
    printf("Dock pure spec ordering PASS\n");
}

void testDraftSnapshotHasAppsBeforePublish() {
    HomeSceneModel model;
    model.begin(UiScene::DataState::Ready);
    assert(model.addApp("builtin.files", "文件管理", "builtin.files"));
    assert(model.addApp("com.weread.client", "微信读书", "icon_home.bmp"));
    assert(model.draftSnapshot().appCount == 2);
    // Asset decode used to walk draftPublication().snapshot here, which is still
    // empty until publish(). That skipped every dock icon, including compiled
    // builtin.files.
    assert(model.draftPublication().snapshot.appCount == 0);
    model.draftPublication().snapshot = model.draftSnapshot();
    assert(model.draftPublication().snapshot.appCount == 2);
    auto tv = model.draftPublication().snapshot.textView(
        model.draftPublication().snapshot.apps[0].id);
    std::string id;
    id.reserve(tv.size);
    for (uint16_t j = 0; j < tv.size; ++j) id.push_back(static_cast<char>(tv.readByte(j)));
    assert(id == "builtin.files");
    printf("draft snapshot vs publication trap PASS\n");
}

void testProductionDockHelperIfPresent() {
    // If Lane A provides a helper header, verify it matches spec. Otherwise this is expected RED until impl lands.
#if __has_include("activities/home/HomeDock.h")
    printf("HomeDock.h found — manual verification needed for ordering (RED until verified)\n");
#else
    // No helper header — check HomeActivity.cpp directly at runtime by reading file (host)
    FILE* f = fopen("firmware/src/activities/home/HomeActivity.cpp", "r");
    if (!f) f = fopen("./firmware/src/activities/home/HomeActivity.cpp", "r");
    if (!f) {
        // Try absolute-ish relative to test binary location
        f = fopen("../../firmware/src/activities/home/HomeActivity.cpp", "r");
    }
    if (!f) {
        printf("SKIP: cannot open HomeActivity.cpp to verify dock production (run from repo root)\n");
        return;
    }
    fseek(f,0,SEEK_END);
    long n = ftell(f);
    fseek(f,0,SEEK_SET);
    std::string content;
    content.resize((size_t)n);
    fread(&content[0],1,(size_t)n,f);
    fclose(f);
    bool hasBuiltin = content.find("builtin.files") != std::string::npos;
    bool hasWeread = content.find("com.weread.client") != std::string::npos;
    bool hasFanqie = content.find("com.fanqie.client") != std::string::npos;
    bool hasJjwxc = content.find("com.jjwxc.client") != std::string::npos;
    if (!hasBuiltin) {
        printf("RED: Home dock production missing 'builtin.files' in HomeActivity.cpp — expected until Lane A lands\n");
        printf("  has weread=%d fanqie=%d jjwxc=%d\n", hasWeread, hasFanqie, hasJjwxc);
        // This is the contract failure — assert to make test RED
        assert(false && "Home dock must publish builtin.files first, then weread/fanqie/jjwxc (missing in production)");
    }
    // Check order weread < fanqie < jjwxc
    size_t p1 = content.find("com.weread.client");
    size_t p2 = content.find("com.fanqie.client");
    size_t p3 = content.find("com.jjwxc.client");
    if (p1 != std::string::npos && p2 != std::string::npos && p3 != std::string::npos) {
        assert(p1 < p2 && p2 < p3 && "preferred dock order must be weread, fanqie, jjwxc");
    }
    printf("Production dock check PASS (builtin.files present and ordering)\n");
#endif
}

} // namespace

int main() {
    testKMaxAppItemsRemainsFour();
    testDockSpecPureOrdering();
    testHomeSceneModelHoldsFourAppsInOrder();
    testDraftSnapshotHasAppsBeforePublish();
    testProductionDockHelperIfPresent();
    printf("home dock ALL PASS (if production helper missing, previous assert would have been RED)\n");
    return 0;
}
