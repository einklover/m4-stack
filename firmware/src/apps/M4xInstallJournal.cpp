#include "apps/M4xInstallJournal.h"

#include "apps/M4xPaths.h"

#include <ArduinoJson.h>
#include <SDCardManager.h>

#include <algorithm>
#include <cstring>

namespace M4xInstallJournal {
namespace {

bool readExactFile(const char* path, std::string& out) {
  out.clear();
  FsFile f;
  if (!SdMan.openFileForRead("M4xJnl", path, f)) return false;
  const size_t n = f.fileSize();
  out.resize(n);
  size_t off = 0;
  while (off < n) {
    const int r = f.read(reinterpret_cast<uint8_t*>(&out[off]), n - off);
    if (r <= 0) {
      f.close();
      out.clear();
      return false;
    }
    off += static_cast<size_t>(r);
  }
  f.close();
  return off == n;
}

// Create-only. openFileForWrite uses O_TRUNC, so refuse an existing path and
// remove only the file this call created.
bool writeExactFile(const char* path, const std::string& body) {
  if (SdMan.exists(path)) return false;
  FsFile f;
  if (!SdMan.openFileForWrite("M4xJnl", path, f)) return false;
  const size_t n = body.size();
  size_t off = 0;
  bool ok = true;
  while (ok && off < n) {
    const size_t chunk = std::min<size_t>(4096, n - off);
    const int w = f.write(reinterpret_cast<const uint8_t*>(body.data() + off), chunk);
    if (w <= 0) ok = false;
    else off += static_cast<size_t>(w);
  }
  if (ok && (off != n || f.getWriteError())) ok = false;
  if (ok && f.fileSize() != n) ok = false;
  if (ok && !f.sync()) ok = false;
  if (!f.close()) ok = false;
  if (!ok) {
    SdMan.remove(path);
    return false;
  }
  FsFile verify;
  if (!SdMan.openFileForRead("M4xJnl", path, verify)) {
    SdMan.remove(path);
    return false;
  }
  const bool sizeOk = verify.fileSize() == n;
  verify.close();
  if (!sizeOk) {
    SdMan.remove(path);
    return false;
  }
  return true;
}

bool copyFileExact(const char* src, const char* dst) {
  std::string body;
  if (!readExactFile(src, body)) return false;
  return writeExactFile(dst, body);
}

bool renameOrCopy(const char* src, const char* dst) {
  if (SdMan.rename(src, dst)) return true;
  // Copy would truncate dst. Callers remove a replaceable destination first.
  if (SdMan.exists(dst)) return false;
  if (!copyFileExact(src, dst)) return false;
  SdMan.remove(src);
  return SdMan.exists(dst);
}

// Valid journal document: parses as JSON object with a "txns" array (may be empty).
bool isValidJournalBody(const std::string& raw) {
  if (raw.empty()) return false;
  JsonDocument doc;
  if (deserializeJson(doc, raw)) return false;
  return doc["txns"].is<JsonArray>();
}

void pushStringArray(JsonArray arr, const std::vector<std::string>& v) {
  for (const auto& s : v) arr.add(s);
}

void readStringArray(JsonArrayConst arr, std::vector<std::string>& out) {
  out.clear();
  for (JsonVariantConst x : arr) {
    if (x.is<const char*>()) out.emplace_back(x.as<const char*>());
  }
}

M4xInstallTxn::JournalRecord parseOne(JsonObjectConst o) {
  M4xInstallTxn::JournalRecord r;
  r.id = o["id"] | "";
  r.phase = M4xInstallTxn::phaseFromName(o["phase"] | "");
  r.installPath = o["installPath"] | "";
  r.stagingPath = o["stagingPath"] | "";
  r.backupPath = o["backupPath"] | "";
  r.hadPriorInstall = o["hadPriorInstall"] | false;
  r.newName = o["newName"] | "";
  r.newVersion = o["newVersion"] | "";
  r.newVersionCode = o["newVersionCode"] | 0;
  r.newEntry = o["newEntry"] | "main.lua";
  r.newIcon = o["newIcon"] | "";
  r.oldEntry = o["oldEntry"] | "main.lua";
  r.oldIcon = o["oldIcon"] | "";
  r.oldVersion = o["oldVersion"] | "";
  r.oldVersionCode = o["oldVersionCode"] | 0;
  if (o["newFiles"].is<JsonArray>()) readStringArray(o["newFiles"].as<JsonArrayConst>(), r.newFiles);
  if (o["newPermissions"].is<JsonArray>()) readStringArray(o["newPermissions"].as<JsonArrayConst>(), r.newPermissions);
  if (o["oldFiles"].is<JsonArray>()) readStringArray(o["oldFiles"].as<JsonArrayConst>(), r.oldFiles);
  return r;
}

void writeOne(JsonObject o, const M4xInstallTxn::JournalRecord& r) {
  o["id"] = r.id;
  o["phase"] = M4xInstallTxn::phaseName(r.phase);
  o["installPath"] = r.installPath;
  o["stagingPath"] = r.stagingPath;
  o["backupPath"] = r.backupPath;
  o["hadPriorInstall"] = r.hadPriorInstall;
  o["newName"] = r.newName;
  o["newVersion"] = r.newVersion;
  o["newVersionCode"] = r.newVersionCode;
  o["newEntry"] = r.newEntry;
  o["newIcon"] = r.newIcon;
  o["oldEntry"] = r.oldEntry;
  o["oldIcon"] = r.oldIcon;
  o["oldVersion"] = r.oldVersion;
  o["oldVersionCode"] = r.oldVersionCode;
  pushStringArray(o["newFiles"].to<JsonArray>(), r.newFiles);
  pushStringArray(o["newPermissions"].to<JsonArray>(), r.newPermissions);
  pushStringArray(o["oldFiles"].to<JsonArray>(), r.oldFiles);
}

std::vector<M4xInstallTxn::JournalRecord> parseBody(const std::string& raw) {
  std::vector<M4xInstallTxn::JournalRecord> out;
  if (!isValidJournalBody(raw)) return out;
  JsonDocument doc;
  if (deserializeJson(doc, raw)) return out;
  for (JsonObject o : doc["txns"].as<JsonArray>()) {
    auto r = parseOne(o);
    if (!r.id.empty() && r.phase != M4xInstallTxn::Phase::Idle) out.push_back(std::move(r));
  }
  return out;
}

// Recoverable journal replace. The new body is written to .tmp.part first.
// A sole valid .tmp is never removed until the replacement is verified.
bool durableWriteJournal(const char* path, const std::string& body) {
  if (!isValidJournalBody(body)) return false;

  const std::string tmp = std::string(path) + ".tmp";
  const std::string bak = std::string(path) + ".bak";
  const std::string part = std::string(path) + ".tmp.part";

  if (SdMan.exists(part.c_str()) && !SdMan.remove(part.c_str())) return false;
  if (!writeExactFile(part.c_str(), body)) {
    if (SdMan.exists(part.c_str())) SdMan.remove(part.c_str());
    return false;
  }

  std::string primaryRaw;
  std::string bakRaw;
  std::string tmpRaw;
  const bool primaryValid = SdMan.exists(path) && readExactFile(path, primaryRaw) && isValidJournalBody(primaryRaw);
  const bool bakValid =
      SdMan.exists(bak.c_str()) && readExactFile(bak.c_str(), bakRaw) && isValidJournalBody(bakRaw);
  const bool tmpValid = SdMan.exists(tmp.c_str()) && readExactFile(tmp.c_str(), tmpRaw) && isValidJournalBody(tmpRaw);
  if (tmpValid && !primaryValid && !bakValid) {
    if (SdMan.exists(path)) {
      if (SdMan.exists(bak.c_str()) && !SdMan.remove(bak.c_str())) {
        SdMan.remove(part.c_str());
        return false;
      }
      if (!renameOrCopy(path, bak.c_str())) {
        SdMan.remove(part.c_str());
        return false;
      }
    }
    if (!renameOrCopy(part.c_str(), path)) {
      SdMan.remove(part.c_str());
      return false;
    }
    std::string check;
    if (!readExactFile(path, check) || check != body || !isValidJournalBody(check)) {
      SdMan.remove(part.c_str());
      return false;
    }
    if (SdMan.exists(tmp.c_str())) SdMan.remove(tmp.c_str());
    if (SdMan.exists(part.c_str())) SdMan.remove(part.c_str());
    return true;
  }

  if (SdMan.exists(tmp.c_str()) && !SdMan.remove(tmp.c_str())) {
    SdMan.remove(part.c_str());
    return false;
  }
  if (!renameOrCopy(part.c_str(), tmp.c_str())) {
    SdMan.remove(part.c_str());
    return false;
  }

  const bool hadPrimary = SdMan.exists(path);

  // 2. Move primary → bak (keep last good journal). Never delete primary first.
  if (hadPrimary) {
    if (SdMan.exists(bak.c_str())) {
      // Previous bak is older than primary; safe to replace bak only after we hold primary.
      SdMan.remove(bak.c_str());
    }
    if (!renameOrCopy(path, bak.c_str())) {
      // Primary still intact (rename failed and copy failed).
      SdMan.remove(tmp.c_str());
      return false;
    }
    // If renameOrCopy used copy+remove, primary is gone and bak holds old content.
  }

  // 3. tmp → primary
  if (!renameOrCopy(tmp.c_str(), path)) {
    // Restore bak → primary if we moved it.
    if (hadPrimary && SdMan.exists(bak.c_str())) {
      renameOrCopy(bak.c_str(), path);
    }
    if (SdMan.exists(tmp.c_str())) SdMan.remove(tmp.c_str());
    return false;
  }

  // 4. Drop bak only after primary is verified present + valid.
  std::string check;
  if (!readExactFile(path, check) || !isValidJournalBody(check)) {
    // Primary bad — restore bak if available.
    if (hadPrimary && SdMan.exists(bak.c_str())) {
      if (SdMan.exists(path)) SdMan.remove(path);
      renameOrCopy(bak.c_str(), path);
    }
    return false;
  }
  if (SdMan.exists(bak.c_str())) SdMan.remove(bak.c_str());
  if (SdMan.exists(tmp.c_str())) SdMan.remove(tmp.c_str());
  if (SdMan.exists(part.c_str())) SdMan.remove(part.c_str());
  return true;
}

// Reconcile primary / bak / tmp using pure decideLoad policy.
std::string loadReconciledRaw() {
  const char* path = M4xInstallTxn::kJournalPath;
  const std::string tmp = std::string(path) + ".tmp";
  const std::string bak = std::string(path) + ".bak";

  std::string primaryBody, bakBody, tmpBody;
  const bool pEx = SdMan.exists(path) && readExactFile(path, primaryBody);
  const bool bEx = SdMan.exists(bak.c_str()) && readExactFile(bak.c_str(), bakBody);
  const bool tEx = SdMan.exists(tmp.c_str()) && readExactFile(tmp.c_str(), tmpBody);

  M4xInstallTxn::JournalFile::Presence pr;
  pr.primary = pEx;
  pr.bak = bEx;
  pr.tmp = tEx;
  M4xInstallTxn::JournalFile::Validity v;
  v.primaryValid = pEx && isValidJournalBody(primaryBody);
  v.bakValid = bEx && isValidJournalBody(bakBody);
  v.tmpValid = tEx && isValidJournalBody(tmpBody);

  const auto src = M4xInstallTxn::JournalFile::decideLoad(pr, v);
  switch (src) {
    case M4xInstallTxn::JournalFile::LoadSource::Primary:
      // Drop incomplete tmp; bak may remain until next successful write.
      if (tEx) SdMan.remove(tmp.c_str());
      return primaryBody;
    case M4xInstallTxn::JournalFile::LoadSource::Tmp:
      // Promote complete tmp → primary without destroying bak until success.
      if (durableWriteJournal(path, tmpBody)) {
        return tmpBody;
      }
      // Fall through: try bak if promote failed
      if (v.bakValid) {
        if (SdMan.exists(path)) SdMan.remove(path);
        if (renameOrCopy(bak.c_str(), path)) return bakBody;
        return bakBody;
      }
      return tmpBody;
    case M4xInstallTxn::JournalFile::LoadSource::Bak:
      if (SdMan.exists(path)) SdMan.remove(path);
      if (!renameOrCopy(bak.c_str(), path)) {
        // Keep bak; return content even if restore rename failed.
        return bakBody;
      }
      if (tEx) SdMan.remove(tmp.c_str());
      return bakBody;
    default:
      return {};
  }
}

}  // namespace

std::vector<M4xInstallTxn::JournalRecord> loadAll() {
  return parseBody(loadReconciledRaw());
}

bool saveAll(const std::vector<M4xInstallTxn::JournalRecord>& recs) {
  JsonDocument doc;
  JsonArray arr = doc["txns"].to<JsonArray>();
  for (const auto& r : recs) {
    JsonObject o = arr.add<JsonObject>();
    writeOne(o, r);
  }
  if (doc.overflowed()) return false;
  std::string out;
  const size_t written = serializeJson(doc, out);
  if (doc.overflowed() || written == 0 || written != out.size()) return false;
  SdMan.mkdir("/system", true);
  return durableWriteJournal(M4xInstallTxn::kJournalPath, out);
}

bool upsert(const M4xInstallTxn::JournalRecord& rec) {
  auto all = loadAll();
  bool found = false;
  for (auto& r : all) {
    if (r.id == rec.id) {
      r = rec;
      found = true;
      break;
    }
  }
  if (!found) all.push_back(rec);
  return saveAll(all);
}

bool remove(const std::string& id) {
  auto all = loadAll();
  all.erase(std::remove_if(all.begin(), all.end(), [&](const M4xInstallTxn::JournalRecord& r) { return r.id == id; }),
            all.end());
  return saveAll(all);
}

M4xInstallTxn::JournalRecord find(const std::string& id) {
  for (const auto& r : loadAll()) {
    if (r.id == id) return r;
  }
  return {};
}

bool readPending(const std::string& id, bool& pending) {
  pending = false;
  if (id.empty()) return false;
  const char* path = M4xInstallTxn::kJournalPath;
  const std::string bak = std::string(path) + ".bak";
  const std::string tmp = std::string(path) + ".tmp";

  auto loadOne = [](const char* p, bool& present, bool& valid, std::string& body) -> bool {
    present = false;
    valid = false;
    body.clear();
    if (!SdMan.exists(p)) return true;
    if (!readExactFile(p, body)) return false;
    present = true;
    valid = isValidJournalBody(body);
    return true;
  };

  M4xInstallTxn::JournalFile::Presence pr;
  M4xInstallTxn::JournalFile::Validity v;
  std::string primaryBody, bakBody, tmpBody;
  if (!loadOne(path, pr.primary, v.primaryValid, primaryBody)) return false;
  if (!loadOne(bak.c_str(), pr.bak, v.bakValid, bakBody)) return false;
  if (!loadOne(tmp.c_str(), pr.tmp, v.tmpValid, tmpBody)) return false;

  if (!pr.primary && !pr.bak && !pr.tmp) return true;

  const std::string* snap = nullptr;
  switch (M4xInstallTxn::JournalFile::decideLoad(pr, v)) {
    case M4xInstallTxn::JournalFile::LoadSource::Primary:
      snap = &primaryBody;
      break;
    case M4xInstallTxn::JournalFile::LoadSource::Tmp:
      snap = &tmpBody;
      break;
    case M4xInstallTxn::JournalFile::LoadSource::Bak:
      snap = &bakBody;
      break;
    default:
      return false;
  }
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, *snap);
  if (err || doc.overflowed() || !doc["txns"].is<JsonArray>()) return false;
  for (JsonVariant v : doc["txns"].as<JsonArray>()) {
    if (!v.is<JsonObjectConst>()) return false;
    JsonObjectConst o = v.as<JsonObjectConst>();
    if (!o["id"].is<const char*>()) return false;
    const char* recId = o["id"].as<const char*>();
    if (recId == nullptr || recId[0] == '\0') return false;
    if (id == recId) pending = true;
  }
  return true;
}

int recoverAll(const RecoveryHooks& hooks) {
  auto all = loadAll();
  int n = 0;
  std::vector<M4xInstallTxn::JournalRecord> remaining;
  remaining.reserve(all.size());

  for (const auto& rec : all) {
    ++n;
    M4xInstallTxn::FsSnapshot fs;
    fs.journalPresent = true;
    if (hooks.liveExists) fs.liveExists = hooks.liveExists(rec.installPath, hooks.ud);
    if (hooks.bakExists) fs.bakExists = hooks.bakExists(rec.backupPath, hooks.ud);
    if (hooks.stagingExists) fs.stagingExists = hooks.stagingExists(rec.stagingPath, hooks.ud);
    if (hooks.registryHasId) fs.registryHasId = hooks.registryHasId(rec.id, hooks.ud);
    if (hooks.registryMatchesNew) fs.registryMatchesNew = hooks.registryMatchesNew(rec, hooks.ud);

    M4xInstallTxn::RecoveryAction act = M4xInstallTxn::decideRecovery(rec, fs);
    M4xInstallTxn::HookResults hr;

    switch (act) {
      case M4xInstallTxn::RecoveryAction::DropStagingClearJournal:
        hr.dropStagingOk = !hooks.dropStaging || hooks.dropStaging(rec, hooks.ud);
        break;

      case M4xInstallTxn::RecoveryAction::RestoreOldFromBak:
        hr.restoreOk = hooks.restoreOldFromBak && hooks.restoreOldFromBak(rec, hooks.ud);
        if (hr.restoreOk && hooks.dropStaging) {
          hr.dropStagingOk = hooks.dropStaging(rec, hooks.ud);
          (void)hr.dropStagingOk;  // best-effort after proven restore
        }
        break;

      case M4xInstallTxn::RecoveryAction::CommitNewRegistryThenCleanup:
        hr.commitOk = hooks.commitNewRegistry && hooks.commitNewRegistry(rec, hooks.ud);
        if (hr.commitOk) {
          if (fs.bakExists) {
            hr.dropBakOk = hooks.dropBak && hooks.dropBak(rec, hooks.ud);
          } else {
            hr.dropBakOk = true;
          }
          if (hooks.dropStaging) {
            (void)hooks.dropStaging(rec, hooks.ud);
          }
        } else {
          // Registry commit failed: restore old if bak exists; else RETAIN (first install).
          const auto fb = M4xInstallTxn::afterFailedRegistryCommit(fs.bakExists);
          if (fb == M4xInstallTxn::RecoveryAction::RestoreOldFromBak) {
            hr.restoreOk = hooks.restoreOldFromBak && hooks.restoreOldFromBak(rec, hooks.ud);
            if (hr.restoreOk && hooks.dropStaging) {
              (void)hooks.dropStaging(rec, hooks.ud);
            }
          } else {
            hr.restoreOk = false;
          }
        }
        break;

      case M4xInstallTxn::RecoveryAction::DropBakClearJournal:
        // decideRecovery only returns this when registryMatchesNew.
        if (!fs.registryMatchesNew) {
          // Defensive: never drop bak without verified match.
          act = M4xInstallTxn::RecoveryAction::RetainJournal;
          hr.dropBakOk = false;
        } else {
          hr.dropBakOk = !fs.bakExists || (hooks.dropBak && hooks.dropBak(rec, hooks.ud));
          if (hooks.dropStaging) (void)hooks.dropStaging(rec, hooks.ud);
        }
        break;

      case M4xInstallTxn::RecoveryAction::ClearJournalOnly:
        break;

      case M4xInstallTxn::RecoveryAction::RetainJournal:
      case M4xInstallTxn::RecoveryAction::None:
      default:
        break;
    }

    const bool clear = M4xInstallTxn::mayClearJournalRecord(act, fs.bakExists, hr);
    if (!clear) remaining.push_back(rec);
  }

  // Persist remaining; failure must not be ignored by callers that care, but we
  // already kept in-memory remaining — if save fails, next boot reloads old journal
  // which may still list cleared records (safe: re-run recovery). Never write empty
  // over a failed path that lost data (durableWriteJournal won't delete primary first).
  if (!saveAll(remaining)) {
    // Leave SD journal as-is from last successful save; records we intended to clear
    // may reappear — safe. Records we retained are still on disk from before.
    Serial.printf("[M4x] recover: journal save of remaining failed (%u kept in mem)\n",
                  static_cast<unsigned>(remaining.size()));
  }
  return n;
}

}  // namespace M4xInstallJournal
