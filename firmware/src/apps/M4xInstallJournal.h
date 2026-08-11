#pragma once

// Device journal I/O for M4x install transactions (ArduinoJson + SD).

#include "apps/M4xInstallTxn.h"

#include <string>
#include <vector>

namespace M4xInstallJournal {

// Load all open transactions (empty if missing/corrupt).
std::vector<M4xInstallTxn::JournalRecord> loadAll();

// Replace entire journal (atomic tmp+rename when possible).
bool saveAll(const std::vector<M4xInstallTxn::JournalRecord>& recs);

// Upsert one record by id. Returns false if persist fails.
bool upsert(const M4xInstallTxn::JournalRecord& rec);

// Remove record by id. Returns false if persist fails.
bool remove(const std::string& id);

// Find by id (empty id if missing).
M4xInstallTxn::JournalRecord find(const std::string& id);

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

}  // namespace M4xInstallJournal
