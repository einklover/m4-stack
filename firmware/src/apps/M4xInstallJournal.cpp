#include "apps/M4xInstallJournal.h"

#include "apps/M4xPaths.h"

#include <ArduinoJson.h>
#include <SDCardManager.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <utility>

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

}  // namespace

JournalParse classifyJournalBody(const std::string& raw) {
  if (raw.empty()) return JournalParse::Corrupt;
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, raw);
  if (err == DeserializationError::NoMemory) return JournalParse::Resource;
  if (err) return JournalParse::Corrupt;
  if (!doc["txns"].is<JsonArray>()) return JournalParse::Corrupt;
  return JournalParse::Valid;
}

namespace {

bool isValidJournalBody(const std::string& raw) {
  return classifyJournalBody(raw) == JournalParse::Valid;
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

}  // namespace

ReconciledLoad selectJournalSnapshot() {
  const char* path = M4xInstallTxn::kJournalPath;
  const std::string tmp = std::string(path) + ".tmp";
  const std::string bak = std::string(path) + ".bak";

  const bool pPresent = SdMan.exists(path);
  const bool bPresent = SdMan.exists(bak.c_str());
  const bool tPresent = SdMan.exists(tmp.c_str());
  if (!pPresent && !bPresent && !tPresent) return {true, true, {}, {}};

  std::string primaryBody, bakBody, tmpBody;
  if (pPresent && !readExactFile(path, primaryBody)) return {};
  if (bPresent && !readExactFile(bak.c_str(), bakBody)) return {};
  if (tPresent && !readExactFile(tmp.c_str(), tmpBody)) return {};

  const JournalParse pClass = pPresent ? classifyJournalBody(primaryBody) : JournalParse::Corrupt;
  const JournalParse bClass = bPresent ? classifyJournalBody(bakBody) : JournalParse::Corrupt;
  const JournalParse tClass = tPresent ? classifyJournalBody(tmpBody) : JournalParse::Corrupt;
  if ((pPresent && pClass == JournalParse::Resource) || (bPresent && bClass == JournalParse::Resource) ||
      (tPresent && tClass == JournalParse::Resource)) {
    return {};
  }

  M4xInstallTxn::JournalFile::Presence pr;
  pr.primary = pPresent;
  pr.bak = bPresent;
  pr.tmp = tPresent;
  M4xInstallTxn::JournalFile::Validity v;
  v.primaryValid = pPresent && pClass == JournalParse::Valid;
  v.bakValid = bPresent && bClass == JournalParse::Valid;
  v.tmpValid = tPresent && tClass == JournalParse::Valid;

  ReconciledLoad out;
  out.source = M4xInstallTxn::JournalFile::decideLoad(pr, v);
  switch (out.source) {
    case M4xInstallTxn::JournalFile::LoadSource::Primary:
      out.raw = primaryBody;
      break;
    case M4xInstallTxn::JournalFile::LoadSource::Tmp:
      out.raw = tmpBody;
      break;
    case M4xInstallTxn::JournalFile::LoadSource::Bak:
      out.raw = bakBody;
      break;
    default:
      return {};
  }
  out.ok = true;
  return out;
}

bool promoteSelectedSnapshot(M4xInstallTxn::JournalFile::LoadSource source, const std::string& raw) {
  const char* path = M4xInstallTxn::kJournalPath;
  const std::string tmp = std::string(path) + ".tmp";
  const std::string bak = std::string(path) + ".bak";
  switch (source) {
    case M4xInstallTxn::JournalFile::LoadSource::Primary:
      if (SdMan.exists(tmp.c_str())) SdMan.remove(tmp.c_str());
      return true;
    case M4xInstallTxn::JournalFile::LoadSource::Tmp:
      // Failure keeps primary, bak, and tmp for a later retry.
      return durableWriteJournal(path, raw);
    case M4xInstallTxn::JournalFile::LoadSource::Bak: {
      if (SdMan.exists(path) && !SdMan.remove(path)) return false;
      if (!renameOrCopy(bak.c_str(), path)) return false;
      if (SdMan.exists(tmp.c_str())) SdMan.remove(tmp.c_str());
      std::string check;
      return readExactFile(path, check) && check == raw;
    }
    default:
      return false;
  }
}

namespace {

std::string loadReconciledRaw() {
  ReconciledLoad selected = selectJournalSnapshot();
  if (!selected.ok || selected.absent) return {};
  if (!promoteSelectedSnapshot(selected.source, selected.raw)) return {};
  return selected.raw;
}

}  // namespace

bool tryLoadAll(std::vector<M4xInstallTxn::JournalRecord>& out) {
  out.clear();
  const ReconciledLoad selected = selectJournalSnapshot();
  if (!selected.ok) return false;
  if (selected.absent || selected.raw.empty()) return true;

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, selected.raw);
  if (err || doc.overflowed() || !doc["txns"].is<JsonArray>()) return false;
  for (JsonObject o : doc["txns"].as<JsonArray>()) {
    auto r = parseOne(o);
    if (!r.id.empty() && r.phase != M4xInstallTxn::Phase::Idle) out.push_back(std::move(r));
  }
  if (!promoteSelectedSnapshot(selected.source, selected.raw)) {
    out.clear();
    return false;
  }
  return true;
}

std::vector<M4xInstallTxn::JournalRecord> loadAll() {
  std::vector<M4xInstallTxn::JournalRecord> out;
  (void)tryLoadAll(out);
  return out;
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
  std::vector<M4xInstallTxn::JournalRecord> all;
  if (!tryLoadAll(all)) return false;
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
  std::vector<M4xInstallTxn::JournalRecord> all;
  if (!tryLoadAll(all)) return false;
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
    const JournalParse parsed = classifyJournalBody(body);
    if (parsed == JournalParse::Resource) return false;
    valid = parsed == JournalParse::Valid;
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
  std::vector<M4xInstallTxn::JournalRecord> all;
  if (!tryLoadAll(all)) {
    Serial.printf("[M4x] recover: journal unreadable, keeping disk unchanged\n");
    return 0;
  }
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
        // decideRecovery only returns this when registryMatchesNew and live exists.
        if (!fs.registryMatchesNew || !fs.liveExists) {
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

namespace {

bool listTreeAt(const std::string& root, const std::string& rel, std::vector<std::string>& files,
                std::vector<std::string>& dirs, int depth) {
  if (depth > 16 || files.size() + dirs.size() > 500) return false;
  std::string path = root;
  if (!rel.empty()) {
    if (path.back() != '/') path += '/';
    path += rel;
  }
  FsFile dir = SdMan.open(path.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return false;
  }
  for (;;) {
    FsFile f = dir.openNextFile();
    if (!f) break;
    char name[128] = {};
    f.getName(name, sizeof(name));
    if (name[0] == '\0' || std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) {
      f.close();
      continue;
    }
    const std::string child = rel.empty() ? std::string(name) : rel + "/" + name;
    const bool isDir = f.isDirectory();
    f.close();
    if (isDir) dirs.push_back(child);
    else files.push_back(child);
  }
  dir.close();
  return true;
}

}  // namespace

bool archiveListTree(const std::string& root, std::vector<std::string>& relPaths) {
  relPaths.clear();
  if (!SdMan.exists(root.c_str())) return true;
  std::vector<std::string> dirs = {""};
  std::vector<std::string> files;
  for (size_t i = 0; i < dirs.size(); ++i) {
    if (!listTreeAt(root, dirs[i], files, dirs, static_cast<int>(i))) return false;
  }
  relPaths.swap(files);
  return relPaths.size() <= 500;
}

bool archiveCopyFileVerified(const std::string& src, const std::string& dst) {
  std::string body;
  if (!readExactFile(src.c_str(), body)) return false;
  for (size_t i = 1; i < dst.size(); ++i) {
    if (dst[i] == '/') SdMan.mkdir(dst.substr(0, i).c_str(), true);
  }
  if (!writeExactFile(dst.c_str(), body)) return false;
  std::string back;
  return readExactFile(dst.c_str(), back) && back == body;
}

bool listPendingJournalIds(std::vector<PendingJournalId>& out,
                           bool (*inRegistry)(const std::string& id, void* ud), void* ud) {
  out.clear();
  const ReconciledLoad selected = selectJournalSnapshot();
  if (!selected.ok) return false;
  if (selected.absent || selected.raw.empty()) return true;
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, selected.raw);
  if (err || doc.overflowed() || !doc["txns"].is<JsonArray>()) return false;
  for (JsonObject o : doc["txns"].as<JsonArray>()) {
    auto r = parseOne(o);
    if (r.id.empty() || r.phase == M4xInstallTxn::Phase::Idle) continue;
    PendingJournalId row;
    row.id = r.id;
    row.inRegistry = inRegistry && inRegistry(r.id, ud);
    out.push_back(std::move(row));
  }
  return true;
}

bool archiveAndRelease(const std::string& id, const ArchiveHooks& hooks, std::string& errorOut) {
  errorOut.clear();
  bool idOk = !id.empty() && id.size() <= 80;
  for (unsigned char c : id) {
    if (!(std::isalnum(c) || c == '.' || c == '_' || c == '-')) idOk = false;
  }
  if (!idOk || !hooks.pathExists || !hooks.listTree || !hooks.copyFile) {
    errorOut = "archive_args";
    return false;
  }
  const ReconciledLoad first = selectJournalSnapshot();
  if (!first.ok || first.absent || first.raw.empty()) {
    errorOut = "journal_unavailable";
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, first.raw) || doc.overflowed() || !doc["txns"].is<JsonArray>()) {
    errorOut = "journal_unreadable";
    return false;
  }
  bool found = false;
  M4xInstallTxn::JournalRecord rec;
  for (JsonObject o : doc["txns"].as<JsonArray>()) {
    auto r = parseOne(o);
    if (r.id == id) {
      rec = std::move(r);
      found = true;
      break;
    }
  }
  if (!found) {
    errorOut = "not_found";
    return false;
  }

  // A failed attempt leaves the directory behind. Keep that partial tree and
  // write the next free slot so the still-pending journal can be released.
  std::string root = std::string("/system/m4x_archive/") + id;
  if (SdMan.exists(root.c_str())) {
    bool placed = false;
    for (int n = 2; n <= 32; ++n) {
      const std::string alt = std::string("/system/m4x_archive/") + id + "-" + std::to_string(n);
      if (!SdMan.exists(alt.c_str())) {
        root = alt;
        placed = true;
        break;
      }
    }
    if (!placed) {
      errorOut = "archive_exists";
      return false;
    }
  }
  const std::string snapPath = root + "/snapshot.json";
  for (size_t i = 1; i < snapPath.size(); ++i) {
    if (snapPath[i] == '/') SdMan.mkdir(snapPath.substr(0, i).c_str(), true);
  }
  if (!writeExactFile(snapPath.c_str(), first.raw)) {
    errorOut = "archive_snapshot";
    return false;
  }
  std::string snapBack;
  if (!readExactFile(snapPath.c_str(), snapBack) || snapBack != first.raw) {
    errorOut = "archive_snapshot";
    return false;
  }

  const std::string aside = rec.installPath + ".m4x_restore_aside";
  const std::pair<const char*, std::string> trees[] = {
      {"live", rec.installPath},
      {"bak", rec.backupPath},
      {"staging", rec.stagingPath},
      {"aside", aside},
  };
  for (const auto& tree : trees) {
    if (tree.second.empty() || !hooks.pathExists(tree.second, hooks.ud)) continue;
    std::vector<std::string> rels;
    if (!hooks.listTree(tree.second, rels, hooks.ud)) {
      errorOut = "archive_list";
      return false;
    }
    for (const auto& rel : rels) {
      const std::string src = tree.second.back() == '/' ? tree.second + rel : tree.second + "/" + rel;
      const std::string dst = root + "/" + tree.first + "/" + rel;
      if (!hooks.copyFile(src, dst, hooks.ud)) {
        errorOut = "archive_copy";
        return false;
      }
    }
  }

  const ReconciledLoad again = selectJournalSnapshot();
  if (!again.ok || again.raw != first.raw) {
    errorOut = "snapshot_changed";
    return false;
  }
  if (!remove(id)) {
    errorOut = "release_failed";
    return false;
  }
  bool pending = true;
  if (!readPending(id, pending) || pending) {
    errorOut = "release_unconfirmed";
    return false;
  }
  return true;
}

}  // namespace M4xInstallJournal
