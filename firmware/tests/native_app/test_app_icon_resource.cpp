#include <cassert>
#include <string>
#include <vector>
#include "apps/M4xManifest.h"
#include "activities/home/HomeSceneAssetDecoder.h"

int main(){
 // Minimal manifest icon extension uses existing 'icon' field; verify contract per M4xPathSafe
 const char* j1 = R"({"id":"com.example.test","name":"Test","version":"1.0","versionCode":1,"icon":"icons/home.png"})";
 auto m1 = M4xParseManifest(j1, strlen(j1));
 assert(m1.valid && m1.icon=="icons/home.png");
 auto p1 = HomeSceneAssetDecoder::resolveAppIconPath("/apps/com.example.test", m1.icon);
 assert(p1=="/apps/com.example.test/icons/home.png");

 // Absent icon -> empty path, fallback placeholder without render I/O
 const char* j2 = R"({"id":"com.example.noicon","name":"NoIcon","version":"1.0","versionCode":1})";
 auto m2 = M4xParseManifest(j2, strlen(j2));
 assert(m2.valid && m2.icon.empty());
 auto p2 = HomeSceneAssetDecoder::resolveAppIconPath("/apps/com.example.noicon", m2.icon);
 assert(p2.empty());

 // Malformed -> rejected, no registry entry, fallback
 const char* j3 = R"({"id":"com.example.bad","name":"Bad","version":"1.0","versionCode":1,"icon":"../evil.png"})";
 auto m3 = M4xParseManifest(j3, strlen(j3));
 assert(!m3.valid);

 // Home asset publication path: valid icon should be attempted via decodeAppIconForPublication (bounded, no render I/O)
 HomeScene::HomeScenePublication pub{};
 UiScene::AssetKey key{34,21,0};
 bool ok = HomeSceneAssetDecoder::decodeAppIconForPublication(pub, "/apps/com.example.test", "icons/home.png", key);
 assert(!ok); // host stub returns false, fallback placeholder used in renderer (drawRect)

 // Valid dimensions still enforced
 HomeScene::HomeScenePublication pub2{};
 uint8_t buf[HomeScene::kHomeAppIconStride*HomeScene::kHomeAppIconH]={};
 bool filled = HomeSceneAssetDecoder::fillFallbackAppIcon(buf, HomeScene::kHomeAppIconW, HomeScene::kHomeAppIconH, HomeScene::kHomeAppIconStride, 0);
 assert(filled);
 UiScene::AssetKey k2{34,21,0};
 bool added = HomeScene::homeAddAssetToPublication(pub2, k2, buf, HomeScene::kHomeAppIconW, HomeScene::kHomeAppIconH, HomeScene::kHomeAppIconStride);
 assert(added);

 puts("app_icon_resource GREEN");
 return 0;
}
