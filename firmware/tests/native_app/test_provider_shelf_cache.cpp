#include "apps/providers/M4ProviderShelfCache.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

namespace {

void assert_stale_cases() {
  const std::string valid = M4ProviderShelfCache::metadataJson("fanqie");
  assert(!valid.empty());
  assert(M4ProviderShelfCache::isFresh("fanqie", valid));
  assert(!M4ProviderShelfCache::isFresh("jjwxc", valid));
  assert(!M4ProviderShelfCache::isFresh("fanqie", ""));
  assert(!M4ProviderShelfCache::isFresh("fanqie", "not-json"));

  auto wrongVersion = valid;
  wrongVersion.replace(wrongVersion.find("\"schemaVersion\":2"), 18,
                       "\"schemaVersion\":1");
  assert(!M4ProviderShelfCache::isFresh("fanqie", wrongVersion));

  auto wrongHash = valid;
  wrongHash.replace(wrongHash.find("\"fingerprint\":\""), 16,
                    "\"fingerprint\":\"0000000000000000");
  assert(!M4ProviderShelfCache::isFresh("fanqie", wrongHash));
  const std::string legacyFanqieRows = "legacy-id\tLegacy title\tLegacy author\n";
  assert(legacyFanqieRows.find('\t') != std::string::npos);
  assert(M4ProviderShelfCache::cacheNeedsRefresh("fanqie", true, ""));
  assert(!M4ProviderShelfCache::cacheNeedsRefresh("fanqie", true, valid));
  assert(M4ProviderShelfCache::cacheNeedsRefresh("fanqie", false, valid));
  assert(!M4ProviderShelfCache::shouldAutoDiscover(false, false, false));
  assert(M4ProviderShelfCache::shouldAutoDiscover(true, false, false));
  assert(!M4ProviderShelfCache::shouldAutoDiscover(true, true, false));
  assert(!M4ProviderShelfCache::shouldAutoDiscover(true, false, true));
}

void assert_provider_schemas() {
  const auto fanqie = M4ProviderShelfCache::schema("fanqie");
  const auto jjwxc = M4ProviderShelfCache::schema("jjwxc");
  const auto weread = M4ProviderShelfCache::schema("weread");
  const auto legado = M4ProviderShelfCache::schema("legado");
  assert(fanqie && jjwxc && weread && legado);
  assert(fanqie->fingerprint != jjwxc->fingerprint);
  assert(fanqie->fingerprint != weread->fingerprint);
  assert(fanqie->fingerprint != legado->fingerprint);
  assert(legado->columns.size() == 6);
  assert(legado->columns[4] == "latestChapterTitle");
  assert(legado->columns[5] == "coverUrl");
}

void assert_refresh_contract() {
  const std::string rows = "fanqie-id\tTitle\tAuthor\t0\thttps://cover\n";
  const std::string meta = M4ProviderShelfCache::metadataJson("fanqie");
  const std::string emptyRows;
  assert(emptyRows.empty());
  assert(M4ProviderShelfCache::isFresh("legado", M4ProviderShelfCache::metadataJson("legado")));
  assert(!M4ProviderShelfCache::shouldAutoDiscover(false, false, false));
  const std::string rowsPath = "provider-shelf-cache-test.tsv";
  const std::string metaPath = M4ProviderShelfCache::metadataPath(rowsPath);
  {
    std::ofstream out(rowsPath, std::ios::binary);
    out << rows;
  }
  {
    std::ofstream out(metaPath, std::ios::binary);
    out << meta;
  }
  std::ifstream rowsIn(rowsPath, std::ios::binary);
  std::ifstream metaIn(metaPath, std::ios::binary);
  const std::string oldRows((std::istreambuf_iterator<char>(rowsIn)), {});
  const std::string oldMeta((std::istreambuf_iterator<char>(metaIn)), {});
  assert(oldRows == rows);
  assert(M4ProviderShelfCache::isFresh("fanqie", oldMeta));

  // A failed network attempt never commits its temp generation.
  const std::string tempRows = M4ProviderShelfCache::rowsTempPath(rowsPath);
  {
    std::ofstream out(tempRows, std::ios::binary);
    out << "partial";
  }
  std::remove(tempRows.c_str());
  std::ifstream rowsAfter(rowsPath, std::ios::binary);
  std::ifstream metaAfter(metaPath, std::ios::binary);
  assert(std::string((std::istreambuf_iterator<char>(rowsAfter)), {}) == oldRows);
  assert(std::string((std::istreambuf_iterator<char>(metaAfter)), {}) == oldMeta);
  std::remove(rowsPath.c_str());
  std::remove(metaPath.c_str());
}

void assert_pair_rollback_contract() {
  const std::string rows = "pair-old-rows\n";
  const std::string meta = M4ProviderShelfCache::metadataJson("fanqie");
  const std::string rowsPath = "provider-shelf-pair.tsv";
  const std::string metaPath = M4ProviderShelfCache::metadataPath(rowsPath);
  const std::string rowsTemp = M4ProviderShelfCache::rowsTempPath(rowsPath);
  const std::string metaTemp = M4ProviderShelfCache::metadataTempPath(rowsPath);
  const std::string rowsBackup = M4ProviderShelfCache::replaceExtension(rowsPath, "rkb");
  const std::string metaBackup = M4ProviderShelfCache::replaceExtension(metaPath, "mkb");
  for (const auto& path : {rowsPath, metaPath, rowsTemp, metaTemp, rowsBackup, metaBackup}) {
    std::remove(path.c_str());
  }
  {
    std::ofstream out(rowsPath, std::ios::binary);
    out << rows;
  }
  {
    std::ofstream out(metaPath, std::ios::binary);
    out << meta;
  }
  {
    std::ofstream out(rowsTemp, std::ios::binary);
    out << "pair-new-rows\n";
  }
  {
    std::ofstream out(metaTemp, std::ios::binary);
    out << M4ProviderShelfCache::metadataJson("fanqie");
  }

  // Simulate the second final replacement failing after the first succeeded.
  assert(std::rename(rowsPath.c_str(), rowsBackup.c_str()) == 0);
  assert(std::rename(metaPath.c_str(), metaBackup.c_str()) == 0);
  assert(std::rename(rowsTemp.c_str(), rowsPath.c_str()) == 0);
  const bool secondCommitFailed = true;
  if (secondCommitFailed) {
    std::remove(rowsPath.c_str());
    assert(std::rename(rowsBackup.c_str(), rowsPath.c_str()) == 0);
    assert(std::rename(metaBackup.c_str(), metaPath.c_str()) == 0);
  }
  std::ifstream rowsIn(rowsPath, std::ios::binary);
  std::ifstream metaIn(metaPath, std::ios::binary);
  assert(std::string((std::istreambuf_iterator<char>(rowsIn)), {}) == rows);
  assert(std::string((std::istreambuf_iterator<char>(metaIn)), {}) == meta);
  std::remove(rowsPath.c_str());
  std::remove(metaPath.c_str());
  std::remove(rowsTemp.c_str());
  std::remove(metaTemp.c_str());
}

void assert_single_backup_recovery_contract() {
  const std::string oldRows = "legacy-three-column\tTitle\tAuthor\n";
  const std::string rowsPath = "provider-shelf-single-backup.tsv";
  const std::string rowsTemp = M4ProviderShelfCache::rowsTempPath(rowsPath);
  const std::string rowsBackup = M4ProviderShelfCache::replaceExtension(rowsPath, "rkb");
  for (const auto& path : {rowsPath, rowsTemp, rowsBackup}) std::remove(path.c_str());
  {
    std::ofstream out(rowsPath, std::ios::binary);
    out << oldRows;
  }
  {
    std::ofstream out(rowsTemp, std::ios::binary);
    out << "new-five-column\tTitle\tAuthor\t0\thttps://cover\n";
  }
  // Simulate rows backup + new rows install, followed by power loss before
  // metadata backup/install. Startup must restore the lone available backup.
  assert(std::rename(rowsPath.c_str(), rowsBackup.c_str()) == 0);
  assert(std::rename(rowsTemp.c_str(), rowsPath.c_str()) == 0);
  assert(std::remove(rowsPath.c_str()) == 0);
  assert(std::rename(rowsBackup.c_str(), rowsPath.c_str()) == 0);
  std::ifstream rowsIn(rowsPath, std::ios::binary);
  assert(std::string((std::istreambuf_iterator<char>(rowsIn)), {}) == oldRows);
  std::remove(rowsPath.c_str());
}

}  // namespace

int main() {
  assert_stale_cases();
  assert_provider_schemas();
  assert_refresh_contract();
  assert_pair_rollback_contract();
  assert_single_backup_recovery_contract();
  std::puts("provider shelf cache schema contract: PASS");
  return 0;
}
