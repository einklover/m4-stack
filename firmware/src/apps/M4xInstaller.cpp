#include "apps/M4xInstaller.h"

#include "apps/M4xInstallJournal.h"
#include "apps/M4xInstallTxn.h"
#include "apps/M4xPathSafe.h"
#include "apps/M4xPaths.h"

#include <SDCardManager.h>
#include <ZipFile.h>
#include <esp_task_wdt.h>

#include <Arduino.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace {

bool writeFileBytes(const char* path, const uint8_t* data, size_t n) {
  if (SdMan.exists(path)) SdMan.remove(path);
  FsFile f;
  if (!SdMan.openFileForWrite("M4x", path, f)) return false;
  size_t off = 0;
  while (off < n) {
    const size_t chunk = std::min<size_t>(4096, n - off);
    const int w = f.write(data + off, chunk);
    if (w <= 0) {
      f.close();
      return false;
    }
    off += static_cast<size_t>(w);
    esp_task_wdt_reset();
    vTaskDelay(1);
  }
  f.close();
  return true;
}

// Best-effort cleanup of an app install / staging tree under validated root.
// Uses inventory paths + deepest-first empty parent removal.
void removeTreeBestEffort(const std::string& root, const M4xManifest* m) {
  if (root.empty()) return;
  auto rm = [&](const std::string& rel) {
    if (rel.empty()) return;
    std::string p = root;
    if (p.back() != '/') p += '/';
    p += rel;
    if (SdMan.exists(p.c_str())) SdMan.remove(p.c_str());
  };

  rm(M4xPaths::kManifestName);
  if (m) {
    rm(m->entry.empty() ? M4xPaths::kEntryDefault : m->entry);
    if (!m->icon.empty()) rm(m->icon);
    for (const auto& f : m->files) rm(f);
  } else {
    rm(M4xPaths::kEntryDefault);
    rm("main.lua");
    rm("icon.png");
    rm("icon.bmp");
  }

  // Remove empty parent directories deepest-first within root.
  if (m) {
    auto rmParentsDeepestFirst = [&](const std::string& rel) {
      std::string cur = rel;
      while (true) {
        const size_t slash = cur.rfind('/');
        if (slash == std::string::npos) break;
        cur = cur.substr(0, slash);
        if (cur.empty()) break;
        std::string d = root;
        if (d.back() != '/') d += '/';
        d += cur;
        // Only remove if still under root and empty enough for rmdir.
        if (d.size() <= root.size()) break;
        if (SdMan.exists(d.c_str())) {
          SdMan.removeDir(d.c_str());
          if (SdMan.exists(d.c_str())) SdMan.remove(d.c_str());
        }
      }
    };
    for (const auto& f : m->files) rmParentsDeepestFirst(f);
    if (!m->icon.empty()) rmParentsDeepestFirst(m->icon);
    if (!m->entry.empty()) rmParentsDeepestFirst(m->entry);
  }

  if (SdMan.exists(root.c_str())) {
    SdMan.removeDir(root.c_str());
    if (SdMan.exists(root.c_str())) SdMan.remove(root.c_str());
  }
}

bool ensureParentDirs(const std::string& outPath, size_t rootLen) {
  for (size_t p = rootLen + 1; p < outPath.size(); ++p) {
    if (outPath[p] == '/') {
      std::string dir = outPath.substr(0, p);
      SdMan.mkdir(dir.c_str(), true);
    }
  }
  return true;
}

// Read one zip entry with size caps. Uses ZipFile inflated size when available.
bool readEntryCapped(const std::string& packagePath, const char* entry, size_t maxBytes,
                     std::vector<uint8_t>& out, std::string& err) {
  out.clear();
  ZipFile zip(packagePath);

  size_t inflated = 0;
  if (zip.getInflatedFileSize(entry, &inflated)) {
    if (inflated > maxBytes) {
      err = std::string("entry_too_large:") + entry;
      return false;
    }
  }

  size_t n = 0;
  uint8_t* raw = zip.readFileToMemory(entry, &n, false);
  if (!raw) {
    err = std::string("missing:") + entry;
    return false;
  }
  if (n > maxBytes) {
    free(raw);
    err = std::string("entry_too_large:") + entry;
    return false;
  }
  out.assign(raw, raw + n);
  free(raw);
  return true;
}

// Stream a STORED (or small) zip entry straight to SD — avoids malloc(whole entry)
// which OOM'd jjwxc main.lua / gbk_table under low free heap during install.
class SdWritePrint final : public Print {
 public:
  explicit SdWritePrint(FsFile& f) : f_(f) {}
  size_t write(uint8_t c) override {
    const int n = f_.write(&c, 1);
    return n > 0 ? 1 : 0;
  }
  size_t write(const uint8_t* buf, size_t size) override {
    size_t off = 0;
    while (off < size) {
      const size_t chunk = std::min<size_t>(4096, size - off);
      const int n = f_.write(buf + off, chunk);
      if (n <= 0) return off;
      off += static_cast<size_t>(n);
      esp_task_wdt_reset();
    }
    return off;
  }

 private:
  FsFile& f_;
};

bool extractEntryToFile(const std::string& packagePath, const char* entry, size_t maxBytes,
                        const std::string& outPath, std::string& err) {
  ZipFile zip(packagePath);
  size_t inflated = 0;
  if (zip.getInflatedFileSize(entry, &inflated)) {
    if (inflated > maxBytes) {
      err = std::string("entry_too_large:") + entry;
      return false;
    }
  }
  // Prefer streaming for any entry that would stress the heap.
  if (inflated == 0 || inflated > 24 * 1024) {
    if (SdMan.exists(outPath.c_str())) SdMan.remove(outPath.c_str());
    FsFile out;
    if (!SdMan.openFileForWrite("M4x", outPath.c_str(), out)) {
      err = std::string("write_failed:") + entry;
      return false;
    }
    SdWritePrint sink(out);
    const bool ok = zip.readFileToStream(entry, sink, 4096);
    out.close();
    if (!ok) {
      SdMan.remove(outPath.c_str());
      err = std::string("missing:") + entry;
      return false;
    }
    return true;
  }
  std::vector<uint8_t> bytes;
  if (!readEntryCapped(packagePath, entry, maxBytes, bytes, err)) return false;
  if (!writeFileBytes(outPath.c_str(), bytes.data(), bytes.size())) {
    err = std::string("write_failed:") + entry;
    return false;
  }
  return true;
}

bool extractListed(const std::string& packagePath, const std::string& destRoot, const M4xManifest& m,
                   M4xInstallResult& r) {
  const auto plan = M4xPathSafe::makeExtractList(m.entry, m.icon, m.files);
  if (!plan.ok) {
    r.error = plan.error;
    r.message = std::string("解压清单无效: ") + plan.error;
    return false;
  }

  size_t total = 0;
  SdMan.mkdir(destRoot.c_str(), true);

  for (const auto& rel : plan.paths) {
    // Re-validate (defense in depth).
    const std::string pathErr = M4xPathSafe::validatePackageRelPath(rel);
    if (!pathErr.empty()) {
      r.error = pathErr;
      r.message = std::string("非法路径: ") + rel;
      return false;
    }

    const size_t cap = M4xPathSafe::maxBytesForEntry(rel, m.entry);
    size_t entryBytes = 0;
    {
      ZipFile zipProbe(packagePath);
      if (!zipProbe.getInflatedFileSize(rel.c_str(), &entryBytes)) {
        if (!m.icon.empty() && rel == m.icon) {
          Serial.printf("[M4x] optional icon missing: %s\n", rel.c_str());
          continue;
        }
        r.error = std::string("missing:") + rel;
        r.message = std::string("解压失败: missing:") + rel;
        return false;
      }
    }
    if (entryBytes == 0 && rel == m.entry) {
      r.error = "empty_entry";
      r.message = "入口脚本为空";
      return false;
    }
    if (entryBytes > cap) {
      r.error = std::string("entry_too_large:") + rel;
      r.message = std::string("解压失败: entry_too_large:") + rel;
      return false;
    }
    if (total + entryBytes > M4xPathSafe::kMaxTotalExtractBytes) {
      r.error = "package_too_large";
      r.message = "安装包解压总量超限";
      return false;
    }

    std::string outPath = destRoot;
    if (!outPath.empty() && outPath.back() != '/') outPath += '/';
    outPath += rel;
    ensureParentDirs(outPath, destRoot.size());
    std::string err;
    if (!extractEntryToFile(packagePath, rel.c_str(), cap, outPath, err)) {
      if (!m.icon.empty() && rel == m.icon) {
        Serial.printf("[M4x] optional icon missing: %s\n", rel.c_str());
        continue;
      }
      r.error = err;
      r.message = std::string("解压失败: ") + err;
      return false;
    }
    total += entryBytes;
    Serial.printf("[M4x] extracted %s (%u bytes)\n", rel.c_str(), static_cast<unsigned>(entryBytes));
    esp_task_wdt_reset();
  }
  return true;
}

// Copy allow-listed files from one install root to another (FAT-safe promote).
bool copyListedFiles(const std::string& fromRoot, const std::string& toRoot, const M4xManifest& m,
                     std::string& errOut) {
  const auto plan = M4xPathSafe::makeExtractList(m.entry, m.icon, m.files);
  if (!plan.ok) {
    errOut = plan.error;
    return false;
  }
  SdMan.mkdir(toRoot.c_str(), true);
  for (const auto& rel : plan.paths) {
    std::string src = fromRoot;
    if (!src.empty() && src.back() != '/') src += '/';
    src += rel;
    if (!SdMan.exists(src.c_str())) {
      if (!m.icon.empty() && rel == m.icon) continue;
      errOut = std::string("missing_staged:") + rel;
      return false;
    }
    FsFile in;
    if (!SdMan.openFileForRead("M4x", src.c_str(), in)) {
      errOut = std::string("open_staged:") + rel;
      return false;
    }
    const size_t n = in.fileSize();
    std::vector<uint8_t> buf(n);
    size_t off = 0;
    while (off < n) {
      const int r = in.read(buf.data() + off, n - off);
      if (r <= 0) {
        in.close();
        errOut = std::string("read_staged:") + rel;
        return false;
      }
      off += static_cast<size_t>(r);
    }
    in.close();

    std::string dst = toRoot;
    if (!dst.empty() && dst.back() != '/') dst += '/';
    dst += rel;
    ensureParentDirs(dst, toRoot.size());
    if (!writeFileBytes(dst.c_str(), buf.data(), buf.size())) {
      errOut = std::string("write_promote:") + rel;
      return false;
    }
    esp_task_wdt_reset();
  }
  return true;
}

// Manifest from inventory paths (registry old entry).
M4xManifest manifestFromInventory(const std::string& entry, const std::string& icon,
                                  const std::vector<std::string>& files) {
  M4xManifest m;
  m.entry = entry.empty() ? M4xPaths::kEntryDefault : entry;
  m.icon = icon;
  m.files = files;
  return m;
}

M4xInstallTxn::JournalRecord makeJournalBase(const M4xManifest& neu, const std::string& dest,
                                             const std::string& staging, const std::string& backup,
                                             const M4xInstalledApp* oldApp) {
  M4xInstallTxn::JournalRecord rec;
  rec.id = neu.id;
  rec.installPath = dest;
  rec.stagingPath = staging;
  rec.backupPath = backup;
  rec.hadPriorInstall = oldApp != nullptr;
  rec.newName = neu.name;
  rec.newVersion = neu.version;
  rec.newVersionCode = neu.versionCode;
  rec.newEntry = neu.entry.empty() ? M4xPaths::kEntryDefault : neu.entry;
  rec.newIcon = neu.icon;
  rec.newFiles = neu.files;
  rec.newPermissions = neu.permissions;
  if (oldApp) {
    rec.oldEntry = oldApp->entry.empty() ? M4xPaths::kEntryDefault : oldApp->entry;
    rec.oldIcon = oldApp->icon;
    rec.oldFiles = oldApp->files;
    rec.oldVersion = oldApp->version;
    rec.oldVersionCode = oldApp->versionCode;
  }
  return rec;
}

bool journalSetPhase(M4xInstallTxn::JournalRecord& rec, M4xInstallTxn::Phase phase) {
  rec.phase = phase;
  return M4xInstallJournal::upsert(rec);
}

// Quarantine using OLD inventory only (not the new package manifest).
bool quarantineLive(const std::string& installPath, const std::string& backup, const M4xManifest& oldInv,
                    std::string& errOut) {
  if (!SdMan.exists(installPath.c_str())) return true;
  if (SdMan.exists(backup.c_str())) removeTreeBestEffort(backup, &oldInv);
  if (SdMan.rename(installPath.c_str(), backup.c_str())) return true;
  if (!copyListedFiles(installPath, backup, oldInv, errOut)) {
    removeTreeBestEffort(backup, &oldInv);
    errOut = errOut.empty() ? "quarantine_copy_failed" : errOut;
    return false;
  }
  std::string bakEntry = backup;
  if (!bakEntry.empty() && bakEntry.back() != '/') bakEntry += '/';
  bakEntry += oldInv.entry.empty() ? M4xPaths::kEntryDefault : oldInv.entry;
  if (!SdMan.exists(bakEntry.c_str())) {
    removeTreeBestEffort(backup, &oldInv);
    errOut = "quarantine_verify_failed";
    return false;
  }
  removeTreeBestEffort(installPath, &oldInv);
  return true;
}

// Promote staging → live. Keeps backup until caller confirms registry success.
// On failure restores backup. Does NOT delete backup on success (caller does after registry).
bool promoteStaging(const std::string& installPath, const std::string& staging, const std::string& backup,
                    const M4xManifest& newManifest, const M4xManifest* oldInvOrNull, std::string& errOut) {
  if (SdMan.exists(backup.c_str()) && oldInvOrNull) {
    removeTreeBestEffort(backup, oldInvOrNull);
  } else if (SdMan.exists(backup.c_str())) {
    removeTreeBestEffort(backup, &newManifest);
  }

  const bool hadLive = SdMan.exists(installPath.c_str());
  if (hadLive) {
    M4xManifest old = oldInvOrNull ? *oldInvOrNull : newManifest;
    if (!quarantineLive(installPath, backup, old, errOut)) {
      removeTreeBestEffort(staging, &newManifest);
      return false;
    }
  }

  if (SdMan.rename(staging.c_str(), installPath.c_str())) {
    return true;  // backup retained until registry OK
  }

  Serial.printf("[M4x] rename staging->dest failed; file-level promote\n");
  if (!copyListedFiles(staging, installPath, newManifest, errOut)) {
    removeTreeBestEffort(installPath, &newManifest);
    if (hadLive && SdMan.exists(backup.c_str())) {
      std::string ignore;
      M4xManifest old = oldInvOrNull ? *oldInvOrNull : newManifest;
      if (!SdMan.rename(backup.c_str(), installPath.c_str())) {
        copyListedFiles(backup, installPath, old, ignore);
      }
      errOut = "promote_failed_restored";
    }
    removeTreeBestEffort(staging, &newManifest);
    return false;
  }
  removeTreeBestEffort(staging, &newManifest);
  return true;
}

// Production recovery hooks for global journal (first-install + upgrade).
struct RecoverUd {};
bool hookLive(const std::string& p, void*) { return SdMan.exists(p.c_str()); }
bool hookBak(const std::string& p, void*) { return SdMan.exists(p.c_str()); }
bool hookStaging(const std::string& p, void*) { return SdMan.exists(p.c_str()); }
bool hookRegHas(const std::string& id, void*) {
  auto apps = M4xRegistry::load();
  return M4xRegistry::find(apps, id) != nullptr;
}
bool hookRegMatch(const M4xInstallTxn::JournalRecord& rec, void*) {
  auto apps = M4xRegistry::load();
  const auto* a = M4xRegistry::find(apps, rec.id);
  return a && a->versionCode == rec.newVersionCode && a->entry == rec.newEntry;
}
bool hookDropStaging(const M4xInstallTxn::JournalRecord& rec, void*) {
  M4xManifest m = manifestFromInventory(rec.newEntry, rec.newIcon, rec.newFiles);
  if (SdMan.exists(rec.stagingPath.c_str())) removeTreeBestEffort(rec.stagingPath, &m);
  // Postcondition: staging gone (or never existed).
  return !SdMan.exists(rec.stagingPath.c_str());
}

// Prove bak is usable before touching live; move live aside rather than destroy first.
bool bakProvenUsable(const M4xInstallTxn::JournalRecord& rec) {
  if (!SdMan.exists(rec.backupPath.c_str())) return false;
  std::string entry = rec.backupPath;
  if (!entry.empty() && entry.back() != '/') entry += '/';
  entry += rec.oldEntry.empty() ? M4xPaths::kEntryDefault : rec.oldEntry;
  return SdMan.exists(entry.c_str());
}

bool hookRestoreOld(const M4xInstallTxn::JournalRecord& rec, void*) {
  M4xManifest oldM = manifestFromInventory(rec.oldEntry, rec.oldIcon, rec.oldFiles);
  M4xManifest newM = manifestFromInventory(rec.newEntry, rec.newIcon, rec.newFiles);
  if (!M4xInstallTxn::mayDestroyLiveForRestore(bakProvenUsable(rec))) return false;

  const std::string aside = rec.installPath + ".m4x_restore_aside";
  if (SdMan.exists(aside.c_str())) removeTreeBestEffort(aside, &newM);

  // Move live aside (recoverable) — do not delete until bak is at live.
  if (SdMan.exists(rec.installPath.c_str())) {
    if (!SdMan.rename(rec.installPath.c_str(), aside.c_str())) {
      std::string ignore;
      if (!copyListedFiles(rec.installPath, aside, newM, ignore)) {
        // Live untouched; bak untouched.
        return false;
      }
      // Only remove live after aside has a copy.
      removeTreeBestEffort(rec.installPath, &newM);
      if (SdMan.exists(rec.installPath.c_str())) {
        // Could not clear live; leave bak intact.
        removeTreeBestEffort(aside, &newM);
        return false;
      }
    }
  }

  std::string ignore;
  bool placed = false;
  if (SdMan.rename(rec.backupPath.c_str(), rec.installPath.c_str())) {
    placed = true;
  } else if (copyListedFiles(rec.backupPath, rec.installPath, oldM, ignore)) {
    placed = true;
  }

  // Verify live entry after restore.
  std::string liveEntry = rec.installPath;
  if (!liveEntry.empty() && liveEntry.back() != '/') liveEntry += '/';
  liveEntry += rec.oldEntry.empty() ? M4xPaths::kEntryDefault : rec.oldEntry;
  if (!placed || !SdMan.exists(liveEntry.c_str())) {
    // Roll back aside → live if possible; keep bak if still present.
    if (SdMan.exists(rec.installPath.c_str())) removeTreeBestEffort(rec.installPath, &oldM);
    if (SdMan.exists(aside.c_str())) {
      if (!SdMan.rename(aside.c_str(), rec.installPath.c_str())) {
        copyListedFiles(aside, rec.installPath, newM, ignore);
      }
    }
    return false;
  }

  // Success: drop aside and leftover bak.
  if (SdMan.exists(aside.c_str())) removeTreeBestEffort(aside, &newM);
  if (SdMan.exists(rec.backupPath.c_str())) removeTreeBestEffort(rec.backupPath, &oldM);
  Serial.printf("[M4x] recover: restored old install for %s\n", rec.id.c_str());
  return true;
}

bool hookCommitReg(const M4xInstallTxn::JournalRecord& rec, void*) {
  auto apps = M4xRegistry::load();
  M4xManifest m;
  m.id = rec.id;
  m.name = rec.newName;
  m.version = rec.newVersion;
  m.versionCode = rec.newVersionCode;
  m.entry = rec.newEntry;
  m.icon = rec.newIcon;
  m.files = rec.newFiles;
  m.permissions = rec.newPermissions;
  m.valid = true;
  M4xRegistry::upsert(apps, m, rec.installPath, static_cast<uint32_t>(millis() / 1000));
  if (!M4xRegistry::save(apps)) return false;
  // Postcondition: registry matches new.
  apps = M4xRegistry::load();
  const auto* a = M4xRegistry::find(apps, rec.id);
  return a && a->versionCode == rec.newVersionCode && a->entry == rec.newEntry;
}

bool hookDropBak(const M4xInstallTxn::JournalRecord& rec, void*) {
  // Never drop bak unless caller already verified registryMatchesNew (recoverAll guards).
  M4xManifest oldM = manifestFromInventory(rec.oldEntry, rec.oldIcon, rec.oldFiles);
  if (SdMan.exists(rec.backupPath.c_str())) removeTreeBestEffort(rec.backupPath, &oldM);
  // Real postcondition: bak must be gone.
  return !SdMan.exists(rec.backupPath.c_str());
}

void recoverInterruptedInstalls() {
  M4xInstallJournal::RecoveryHooks h;
  h.liveExists = &hookLive;
  h.bakExists = &hookBak;
  h.stagingExists = &hookStaging;
  h.registryHasId = &hookRegHas;
  h.registryMatchesNew = &hookRegMatch;
  h.dropStaging = &hookDropStaging;
  h.restoreOldFromBak = &hookRestoreOld;
  h.commitNewRegistry = &hookCommitReg;
  h.dropBak = &hookDropBak;
  const int n = M4xInstallJournal::recoverAll(h);
  if (n > 0) Serial.printf("[M4x] recover: processed %d journal record(s)\n", n);
}

}  // namespace

void M4xInstaller::ensureLayout() {
  SdMan.mkdir(M4xPaths::kAppsRoot, true);
  SdMan.mkdir(M4xPaths::kAppsDataRoot, true);
  SdMan.mkdir(M4xPaths::kInbox, true);
  SdMan.mkdir("/system", true);
  recoverInterruptedInstalls();
}

M4xInstallResult M4xInstaller::probe(const std::string& packagePath) {
  M4xInstallResult r;
  ensureLayout();
  esp_task_wdt_reset();

  if (!SdMan.exists(packagePath.c_str())) {
    r.error = "not_found";
    r.message = "安装包不存在";
    return r;
  }

  std::vector<uint8_t> manifestBytes;
  std::string err;
  if (!readEntryCapped(packagePath, M4xPaths::kManifestName, M4xPathSafe::kMaxManifestBytes, manifestBytes, err)) {
    r.error = err.find("too_large") != std::string::npos ? "manifest_too_large" : "missing_manifest";
    r.message = r.error == "manifest_too_large" ? "manifest.json 过大" : "安装包缺少 manifest.json";
    Serial.printf("[M4x] probe fail: %s path=%s\n", err.c_str(), packagePath.c_str());
    return r;
  }

  r.manifest = M4xParseManifest(reinterpret_cast<const char*>(manifestBytes.data()), manifestBytes.size());
  if (!r.manifest.valid) {
    r.error = r.manifest.error.empty() ? "bad_manifest" : r.manifest.error;
    r.message = std::string("清单无效: ") + r.error;
    return r;
  }

  // Verify every required extract path exists and fits caps (icon optional).
  const auto plan = M4xPathSafe::makeExtractList(r.manifest.entry, r.manifest.icon, r.manifest.files);
  if (!plan.ok) {
    r.error = plan.error;
    r.message = std::string("清单无效: ") + plan.error;
    return r;
  }

  size_t total = 0;
  for (const auto& rel : plan.paths) {
    const size_t cap = M4xPathSafe::maxBytesForEntry(rel, r.manifest.entry);
    size_t inflated = 0;
    ZipFile zip(packagePath);
    const bool hasSize = zip.getInflatedFileSize(rel.c_str(), &inflated);
    if (!hasSize) {
      // Fall back to reading (still capped).
      std::vector<uint8_t> bytes;
      std::string e2;
      if (!readEntryCapped(packagePath, rel.c_str(), cap, bytes, e2)) {
        if (!r.manifest.icon.empty() && rel == r.manifest.icon) continue;
        r.error = e2;
        r.message = std::string("缺少文件: ") + rel;
        return r;
      }
      inflated = bytes.size();
    } else if (inflated > cap) {
      r.error = std::string("entry_too_large:") + rel;
      r.message = std::string("文件过大: ") + rel;
      return r;
    } else {
      // Confirm presence with a cheap open via size already implies entry exists
      // in ZipFile implementation; still ensure entry non-empty for main script.
      if (rel == r.manifest.entry && inflated == 0) {
        r.error = "empty_entry";
        r.message = "入口脚本为空";
        return r;
      }
    }
    if (total + inflated > M4xPathSafe::kMaxTotalExtractBytes) {
      r.error = "package_too_large";
      r.message = "安装包解压总量超限";
      return r;
    }
    total += inflated;
  }

  r.ok = true;
  r.message = "OK";
  r.installPath = std::string(M4xPaths::kAppsRoot) + "/" + r.manifest.id;
  Serial.printf("[M4x] probe ok id=%s v=%s files=%u\n", r.manifest.id.c_str(), r.manifest.version.c_str(),
                static_cast<unsigned>(plan.paths.size()));
  return r;
}

M4xInstallResult M4xInstaller::install(const std::string& packagePath) {
  Serial.printf("[M4x] install begin path=%s freeHeap=%u\n", packagePath.c_str(),
                static_cast<unsigned>(ESP.getFreeHeap()));
  esp_task_wdt_reset();

  M4xInstallResult r = probe(packagePath);
  if (!r.ok) return r;

  auto apps = M4xRegistry::load();
  if (const auto* existing = M4xRegistry::find(apps, r.manifest.id)) {
    if (r.manifest.versionCode < existing->versionCode) {
      r.ok = false;
      r.error = "downgrade";
      r.message = "已安装更高版本，拒绝降级";
      return r;
    }
  }

  const std::string dest = r.installPath;
  const std::string staging = M4xPathSafe::stagingDirFor(dest);
  const std::string backup = M4xPathSafe::backupDirFor(dest);

  // Old inventory from registry (for quarantine of previous version files).
  M4xManifest oldInv;
  const M4xInstalledApp* existingApp = M4xRegistry::find(apps, r.manifest.id);
  if (existingApp) {
    oldInv = manifestFromInventory(existingApp->entry, existingApp->icon, existingApp->files);
  }

  if (SdMan.exists(staging.c_str())) {
    removeTreeBestEffort(staging, &r.manifest);
  }

  M4xInstallTxn::JournalRecord jrec =
      makeJournalBase(r.manifest, dest, staging, backup, existingApp);
  jrec.phase = M4xInstallTxn::Phase::Staging;
  if (!M4xInstallJournal::upsert(jrec)) {
    r.ok = false;
    r.error = "journal_write";
    r.message = "无法写入安装事务日志";
    return r;
  }

  if (!extractListed(packagePath, staging, r.manifest, r)) {
    removeTreeBestEffort(staging, &r.manifest);
    M4xInstallJournal::remove(r.manifest.id);
    r.ok = false;
    return r;
  }
  if (!journalSetPhase(jrec, M4xInstallTxn::Phase::Staged)) {
    removeTreeBestEffort(staging, &r.manifest);
    M4xInstallJournal::remove(r.manifest.id);
    r.ok = false;
    r.error = "journal_write";
    r.message = "安装日志更新失败（未改动已安装版本）";
    return r;
  }

  // Live mutation only after staged is durable in journal.
  std::string promoteErr;
  const M4xManifest* oldPtr = existingApp ? &oldInv : nullptr;
  if (existingApp) {
    if (!journalSetPhase(jrec, M4xInstallTxn::Phase::Quarantined)) {
      removeTreeBestEffort(staging, &r.manifest);
      M4xInstallJournal::remove(r.manifest.id);
      r.ok = false;
      r.error = "journal_write";
      r.message = "安装日志更新失败（未隔离旧版本）";
      return r;
    }
  }

  if (!promoteStaging(dest, staging, backup, r.manifest, oldPtr, promoteErr)) {
    // Fail-closed: do not restore/clear journal here. Last durable phase
    // (Staged/Quarantined) + live/bak/staging stay for boot recoverAll.
    r.ok = false;
    r.error = promoteErr;
    r.message = "切换安装目录失败（保留事务日志，重启恢复）";
    Serial.printf("[M4x] install promote fail (fail-closed, journal retained) id=%s\n",
                  r.manifest.id.c_str());
    return r;
  }
  if (!journalSetPhase(jrec, M4xInstallTxn::Phase::LiveSwitched)) {
    // Live may already be new; durable journal still prior phase (Quarantined/Staged).
    // Fail-closed: no restore, no journal remove — boot uses last durable phase.
    r.ok = false;
    r.error = "journal_write_after_switch";
    r.message = "安装日志失败（保留上一阶段事务日志，重启恢复）";
    Serial.printf("[M4x] install LiveSwitched journal fail (fail-closed) id=%s\n", r.manifest.id.c_str());
    return r;
  }

  const std::string dataDir = std::string(M4xPaths::kAppsDataRoot) + "/" + r.manifest.id;
  SdMan.mkdir(dataDir.c_str(), true);

  M4xRegistry::upsert(apps, r.manifest, dest, static_cast<uint32_t>(millis() / 1000));
  if (!M4xRegistry::save(apps)) {
    // Fail-closed: keep new live + old bak + LiveSwitched journal for boot.
    // Do not restore or clear journal (avoids LiveSwitched-after-restore CommitNew bug).
    r.ok = false;
    r.error = "registry_write";
    r.message = "写入应用注册表失败（保留 LiveSwitched 事务日志，重启恢复）";
    Serial.printf("[M4x] install registry save fail (fail-closed) id=%s\n", r.manifest.id.c_str());
    return r;
  }

  if (!journalSetPhase(jrec, M4xInstallTxn::Phase::RegistryCommitted)) {
    // Registry already durable — still drop bak; journal may be cleaned on next boot.
    Serial.printf("[M4x] journal commit phase write failed (registry OK)\n");
  }

  if (SdMan.exists(backup.c_str())) {
    removeTreeBestEffort(backup, existingApp ? &oldInv : &r.manifest);
  }
  M4xInstallJournal::remove(r.manifest.id);

  r.ok = true;
  r.message = "安装成功";
  Serial.printf("[M4x] install ok %s -> %s freeHeap=%u\n", r.manifest.id.c_str(), dest.c_str(),
                static_cast<unsigned>(ESP.getFreeHeap()));
  return r;
}

bool M4xInstaller::uninstall(const std::string& id, bool clearData, std::string& errorOut) {
  if (!M4xIsValidPackageId(id)) {
    errorOut = "invalid_id";
    return false;
  }
  auto apps = M4xRegistry::load();
  const auto* app = M4xRegistry::find(apps, id);
  if (!app) {
    errorOut = "not_installed";
    return false;
  }
  const std::string path = app->path;
  M4xManifest stub;
  stub.entry = app->entry;
  stub.icon = app->icon;
  stub.files = app->files;  // inventory from registry for complete cleanup
  M4xRegistry::remove(apps, id);
  if (!M4xRegistry::save(apps)) {
    errorOut = "registry_write";
    return false;
  }
  removeTreeBestEffort(path, &stub);
  removeTreeBestEffort(M4xPathSafe::stagingDirFor(path), &stub);
  removeTreeBestEffort(M4xPathSafe::backupDirFor(path), &stub);
  if (clearData) {
    const std::string dataDir = std::string(M4xPaths::kAppsDataRoot) + "/" + id;
    if (SdMan.exists(dataDir.c_str())) {
      // Known roots + nested cache tree (WeRead: cache/<bookId>/...).
      const char* tops[] = {"config.json", "progress.json", "shelf_cache.json", "state.json", nullptr};
      for (int i = 0; tops[i]; ++i) {
        std::string p = dataDir + "/" + tops[i];
        if (SdMan.exists(p.c_str())) SdMan.remove(p.c_str());
      }
      // Best-effort: remove cache/ tree files via recursive enumerator if available.
      std::string cacheDir = dataDir + "/cache";
      if (SdMan.exists(cacheDir.c_str())) {
        // RemoveDir is recursive in SDCardManager for known trees.
        SdMan.removeDir(cacheDir.c_str());
        if (SdMan.exists(cacheDir.c_str())) SdMan.remove(cacheDir.c_str());
      }
      SdMan.removeDir(dataDir.c_str());
      if (SdMan.exists(dataDir.c_str())) SdMan.remove(dataDir.c_str());
    }
  }
  return true;
}

std::string M4xInstaller::entryScriptPath(const M4xInstalledApp& app) {
  std::string p = app.path;
  if (!p.empty() && p.back() != '/') p += '/';
  p += app.entry.empty() ? M4xPaths::kEntryDefault : app.entry;
  return p;
}
