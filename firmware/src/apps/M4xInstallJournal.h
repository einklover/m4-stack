#pragma once

// Device journal I/O for M4x install transactions (ArduinoJson + SD).

#include "apps/M4xInstallTxn.h"

#include <string>
#include <vector>

namespace M4xInstallJournal {

// JsonDocument outcome for one journal file. Resource is not corruption:
// NoMemory must not lose priority to an older bak/tmp.
enum class JournalParse : int { Valid = 0, Corrupt = 1, Resource = 2 };

JournalParse classifyJournalBody(const std::string& raw);

struct ReconciledLoad {
  bool ok = false;
  bool absent = false;
  std::string raw;
  M4xInstallTxn::JournalFile::LoadSource source = M4xInstallTxn::JournalFile::LoadSource::None;
};

// Read-only. No delete, promote, or save.
ReconciledLoad selectJournalSnapshot();

// After a successful parse of the selected body. Tmp promotion failure
// does not fall through to an older bak.
bool promoteSelectedSnapshot(M4xInstallTxn::JournalFile::LoadSource source, const std::string& raw);

// Load open transactions. False: snapshot unreadable, resource failure, or
// the selected body failed to parse. Disk is left unchanged on false.
bool tryLoadAll(std::vector<M4xInstallTxn::JournalRecord>& out);

// Load all open transactions (empty if missing). Prefer tryLoadAll.
std::vector<M4xInstallTxn::JournalRecord> loadAll();

// Replace entire journal (atomic tmp+rename when possible).
bool saveAll(const std::vector<M4xInstallTxn::JournalRecord>& recs);

// Upsert one record by id. Returns false if persist fails.
bool upsert(const M4xInstallTxn::JournalRecord& rec);

// Remove record by id. Returns false if persist fails.
bool remove(const std::string& id);

// Find by id (empty id if missing). Reads the whole journal; there is no id index.
M4xInstallTxn::JournalRecord find(const std::string& id);

// Read-only same-id check against the single JournalFile::decideLoad snapshot.
// Does not promote, delete, or rewrite. Returns false when a primary/bak/tmp
// file exists but cannot be read, or when files exist but decideLoad is None.
// *pending is meaningful only when this returns true.
bool readPending(const std::string& id, bool& pending);

// Boot recovery: apply decideRecovery for every journal entry via callbacks.
struct RecoveryHooks {
  // FS probes
  bool (*liveExists)(const std::string& path, void* ud) = nullptr;
  bool (*bakExists)(const std::string& path, void* ud) = nullptr;
  bool (*stagingExists)(const std::string& path, void* ud) = nullptr;
  bool (*registryMatchesNew)(const M4xInstallTxn::JournalRecord& rec, void* ud) = nullptr;
  bool (*registryHasId)(const std::string& id, void* ud) = nullptr;
  // Actions (return false on failure)
  bool (*dropStaging)(const M4xInstallTxn::JournalRecord& rec, void* ud) = nullptr;
  bool (*restoreOldFromBak)(const M4xInstallTxn::JournalRecord& rec, void* ud) = nullptr;
  bool (*commitNewRegistry)(const M4xInstallTxn::JournalRecord& rec, void* ud) = nullptr;
  bool (*dropBak)(const M4xInstallTxn::JournalRecord& rec, void* ud) = nullptr;
  void* ud = nullptr;
};

// Executes production recovery decisions for all journal records.
// Returns number of records processed.
int recoverAll(const RecoveryHooks& hooks);

struct PendingJournalId {
  std::string id;
  bool inRegistry = false;
};

// Pending journal ids from the read-only snapshot. False on resource,
// read, or parse failure. Includes ids that are absent from the registry.
bool listPendingJournalIds(std::vector<PendingJournalId>& out,
                           bool (*inRegistry)(const std::string& id, void* ud), void* ud);

struct ArchiveHooks {
  bool (*pathExists)(const std::string& path, void* ud) = nullptr;
  bool (*listTree)(const std::string& root, std::vector<std::string>& relPaths, void* ud) = nullptr;
  bool (*copyFile)(const std::string& src, const std::string& dst, void* ud) = nullptr;
  void* ud = nullptr;
};

// User confirmation entry. Archives the full snapshot and any live, bak,
// staging, or restore-aside tree, verifies read-back, then removes that
// journal id. Does not change the registry or /apps_data.
bool archiveAndRelease(const std::string& id, const ArchiveHooks& hooks, std::string& errorOut);

// Recursive file list and verified file copy used by the installer entry.
bool archiveListTree(const std::string& root, std::vector<std::string>& relPaths);
bool archiveCopyFileVerified(const std::string& src, const std::string& dst);

}  // namespace M4xInstallJournal
