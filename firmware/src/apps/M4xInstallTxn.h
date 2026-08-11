#pragma once

// Global install transaction journal + pure recovery decisions (host-testable).
// Production M4xInstaller persists records under /system/m4x_install_journal.json
// and executes decideRecovery() / applyRecoveryHooks() on boot / ensureLayout.

#include <string>
#include <vector>

namespace M4xInstallTxn {

// Journal path independent of app registry (first-install visible).
inline constexpr const char* kJournalPath = "/system/m4x_install_journal.json";

enum class Phase : int {
  Idle = 0,
  Staging = 1,            // extracting to .staging (live untouched)
  Staged = 2,             // staging complete
  Quarantined = 3,        // old install moved to .bak (upgrade only)
  LiveSwitched = 4,       // new files at live; registry NOT durable yet
  RegistryCommitted = 5,  // registry matches new; bak may be deleted
};

inline const char* phaseName(Phase p) {
  switch (p) {
    case Phase::Staging:
      return "staging";
    case Phase::Staged:
      return "staged";
    case Phase::Quarantined:
      return "quarantined";
    case Phase::LiveSwitched:
      return "live_switched";
    case Phase::RegistryCommitted:
      return "registry_committed";
    default:
      return "idle";
  }
}

inline Phase phaseFromName(const std::string& s) {
  if (s == "staging") return Phase::Staging;
  if (s == "staged") return Phase::Staged;
  if (s == "quarantined") return Phase::Quarantined;
  if (s == "live_switched") return Phase::LiveSwitched;
  if (s == "registry_committed") return Phase::RegistryCommitted;
  return Phase::Idle;
}

// Enough metadata to restore old or finish new registry commit.
struct JournalRecord {
  std::string id;
  Phase phase = Phase::Idle;
  std::string installPath;
  std::string stagingPath;
  std::string backupPath;
  bool hadPriorInstall = false;

  std::string newName;
  std::string newVersion;
  int newVersionCode = 0;
  std::string newEntry = "main.lua";
  std::string newIcon;
  std::vector<std::string> newFiles;
  std::vector<std::string> newPermissions;

  std::string oldEntry = "main.lua";
  std::string oldIcon;
  std::vector<std::string> oldFiles;
  std::string oldVersion;
  int oldVersionCode = 0;
};

struct FsSnapshot {
  bool liveExists = false;
  bool bakExists = false;
  bool stagingExists = false;
  bool journalPresent = false;
  // True if registry currently has this id with newVersionCode (durable match).
  bool registryMatchesNew = false;
  bool registryHasId = false;
};

enum class RecoveryAction : int {
  None = 0,
  // Drop staging only; live untouched. Clear journal only after drop succeeds.
  DropStagingClearJournal,
  // Restore bak → live using old inventory; clear only after restore succeeds.
  RestoreOldFromBak,
  // Finish registry commit from journal new metadata, then drop bak.
  // Clear only after commit + required cleanup succeed.
  CommitNewRegistryThenCleanup,
  // Drop bak only when registry already matches new. Clear only after drop ok.
  DropBakClearJournal,
  // Corrupt/empty id: safe to drop the journal entry itself.
  ClearJournalOnly,
  // Keep journal record unchanged (retry later). Never invent "orphan ok".
  RetainJournal,
};

// Pure recovery decision — production and tests share this function.
// Policy: restore old unless the new registry commit is known durable.
// NEVER drop bak when registryMatchesNew is false.
inline RecoveryAction decideRecovery(const JournalRecord& rec, const FsSnapshot& fs) {
  if (rec.id.empty() || rec.phase == Phase::Idle) return RecoveryAction::ClearJournalOnly;

  // Once the new registry entry is durable, only cleanup remains.
  if (fs.registryMatchesNew) {
    return RecoveryAction::DropBakClearJournal;
  }

  switch (rec.phase) {
    case Phase::Staging:
      if (rec.hadPriorInstall && !fs.liveExists && fs.bakExists) {
        return RecoveryAction::RestoreOldFromBak;
      }
      return RecoveryAction::DropStagingClearJournal;

    case Phase::Staged: {
      if (!rec.hadPriorInstall && fs.liveExists && rec.newVersionCode > 0 && !rec.newEntry.empty()) {
        return RecoveryAction::CommitNewRegistryThenCleanup;
      }
      if (rec.hadPriorInstall && !fs.liveExists && fs.bakExists) {
        return RecoveryAction::RestoreOldFromBak;
      }
      return RecoveryAction::DropStagingClearJournal;
    }

    case Phase::Quarantined:
      if (fs.bakExists) return RecoveryAction::RestoreOldFromBak;
      return RecoveryAction::DropStagingClearJournal;

    case Phase::LiveSwitched:
      // Upgrade without bak: never CommitNew. bak was already consumed (e.g. a failed
      // immediate rollback that left LiveSwitched) or lost — new metadata must not be
      // written over possibly-old live files. Boot retains until reinstall/manual fix.
      if (rec.hadPriorInstall && !fs.bakExists) {
        return RecoveryAction::RetainJournal;
      }
      // First install, or upgrade with bak still present: finish new registry when live+meta ok.
      if (fs.liveExists && rec.newVersionCode > 0 && !rec.newEntry.empty()) {
        return RecoveryAction::CommitNewRegistryThenCleanup;
      }
      if (fs.bakExists) return RecoveryAction::RestoreOldFromBak;
      if (fs.liveExists) return RecoveryAction::RetainJournal;
      return RecoveryAction::DropStagingClearJournal;

    case Phase::RegistryCommitted:
      // registryMatchesNew is false: NEVER drop bak. Retry commit or retain.
      if (fs.liveExists && rec.newVersionCode > 0 && !rec.newEntry.empty()) {
        return RecoveryAction::CommitNewRegistryThenCleanup;
      }
      return RecoveryAction::RetainJournal;

    default:
      return RecoveryAction::ClearJournalOnly;
  }
}

// After a failed registry commit attempt: restore old if bak exists; else RETAIN journal.
// Never ClearJournalOnly here — that discards the only retry metadata.
inline RecoveryAction afterFailedRegistryCommit(bool bakExists) {
  if (bakExists) return RecoveryAction::RestoreOldFromBak;
  return RecoveryAction::RetainJournal;
}

inline RecoveryAction decideRecoveryWithCommitResult(const JournalRecord& rec, const FsSnapshot& fs,
                                                     bool registryCommitOk) {
  RecoveryAction a = decideRecovery(rec, fs);
  if (a == RecoveryAction::CommitNewRegistryThenCleanup && !registryCommitOk) {
    return afterFailedRegistryCommit(fs.bakExists);
  }
  return a;
}

// ---- Recovery execution: clear record only when every required step succeeds ----

struct HookResults {
  bool dropStagingOk = true;
  bool restoreOk = true;
  bool commitOk = true;
  bool dropBakOk = true;
  bool journalSaveOk = true;
};

// Given the decided action and injected hook outcomes, may the journal record be cleared?
// Production recoverAll must use this exact policy.
inline bool mayClearJournalRecord(RecoveryAction act, bool bakExists, const HookResults& hr) {
  switch (act) {
    case RecoveryAction::DropStagingClearJournal:
      return hr.dropStagingOk;
    case RecoveryAction::RestoreOldFromBak:
      // Staging cleanup is best-effort after a proven restore; restore itself is mandatory.
      return hr.restoreOk;
    case RecoveryAction::CommitNewRegistryThenCleanup:
      if (!hr.commitOk) {
        // Commit failed: clear only if we successfully restored old (upgrade path).
        if (bakExists) return hr.restoreOk;
        return false;  // first install: always retain for retry
      }
      // Commit succeeded: drop bak must succeed before clear (postcondition).
      // dropStaging is best-effort; bak drop is mandatory when bak existed.
      if (bakExists) return hr.dropBakOk;
      return true;
    case RecoveryAction::DropBakClearJournal:
      // Only reached when registryMatchesNew; bak drop must verify absence.
      return hr.dropBakOk;
    case RecoveryAction::ClearJournalOnly:
      return true;
    case RecoveryAction::RetainJournal:
    case RecoveryAction::None:
    default:
      return false;
  }
}

// ---- Durable journal file protocol (pure; production I/O follows these steps) ----

namespace JournalFile {

enum class LoadSource : int { None = 0, Primary = 1, Bak = 2, Tmp = 3 };

struct Presence {
  bool primary = false;
  bool bak = false;
  bool tmp = false;
};

struct Validity {
  bool primaryValid = false;
  bool bakValid = false;
  bool tmpValid = false;
};

// Prefer primary; if missing/invalid, prefer complete tmp (new write); else bak.
inline LoadSource decideLoad(const Presence& p, const Validity& v) {
  if (p.primary && v.primaryValid) return LoadSource::Primary;
  if (p.tmp && v.tmpValid) return LoadSource::Tmp;
  if (p.bak && v.bakValid) return LoadSource::Bak;
  return LoadSource::None;
}

// Ordered write steps. Never delete last valid primary before replacement exists.
enum class WriteStep : int {
  WriteTmp = 0,
  MovePrimaryToBak = 1,  // only if primary exists; primary becomes bak
  MoveTmpToPrimary = 2,
  DropBak = 3,
  Done = 4,
};

inline WriteStep nextWriteStep(WriteStep cur, bool primaryExisted) {
  switch (cur) {
    case WriteStep::WriteTmp:
      return primaryExisted ? WriteStep::MovePrimaryToBak : WriteStep::MoveTmpToPrimary;
    case WriteStep::MovePrimaryToBak:
      return WriteStep::MoveTmpToPrimary;
    case WriteStep::MoveTmpToPrimary:
      return WriteStep::DropBak;
    case WriteStep::DropBak:
      return WriteStep::Done;
    default:
      return WriteStep::Done;
  }
}

// After a crash at a given completed step, which load source recovers?
// completedStep = last step that fully finished.
inline LoadSource loadAfterCrash(WriteStep completedStep, bool primaryExisted, bool tmpValid, bool bakValid,
                                 bool primaryValid) {
  Presence p;
  Validity v;
  // Reconstruct presence from completed steps.
  switch (completedStep) {
    case WriteStep::WriteTmp:
      // primary still old (if existed), tmp new, bak maybe stale
      p.primary = primaryExisted;
      p.tmp = true;
      p.bak = false;
      v.primaryValid = primaryExisted;
      v.tmpValid = tmpValid;
      break;
    case WriteStep::MovePrimaryToBak:
      // primary gone, bak=old primary, tmp=new
      p.primary = false;
      p.bak = true;
      p.tmp = true;
      v.bakValid = bakValid;
      v.tmpValid = tmpValid;
      break;
    case WriteStep::MoveTmpToPrimary:
      // primary=new, bak=old (may still exist)
      p.primary = true;
      p.bak = primaryExisted;
      p.tmp = false;
      v.primaryValid = primaryValid;
      v.bakValid = bakValid;
      break;
    case WriteStep::DropBak:
    case WriteStep::Done:
      p.primary = true;
      v.primaryValid = primaryValid;
      break;
    default:
      break;
  }
  return decideLoad(p, v);
}

}  // namespace JournalFile

// Marker/journal write failure must abort before live mutation.
inline bool mayMutateLive(Phase current, bool journalWriteOk) {
  if (!journalWriteOk) return false;
  return current == Phase::Staged || current == Phase::Quarantined || current == Phase::LiveSwitched ||
         current == Phase::RegistryCommitted;
}

inline bool mayStartQuarantine(Phase current, bool journalWriteOk) {
  return journalWriteOk && current == Phase::Staged;
}

inline bool mayWriteLive(bool liveExists, bool quarantineOk) {
  if (!liveExists) return true;
  return quarantineOk;
}

inline bool shouldRestoreBackup(bool promoteOk, bool hadBackup) {
  return !promoteOk && hadBackup;
}

// Restore must not destroy live before bak is proven usable.
inline bool mayDestroyLiveForRestore(bool bakProvenUsable) { return bakProvenUsable; }

// Drop bak only when registry is verified to match new.
inline bool mayDropBak(bool registryMatchesNew) { return registryMatchesNew; }

// ---- Immediate install() failure branches: fail-closed ----
// promote / LiveSwitched journal write / registry save failures must NOT restore,
// drop bak, or remove/update the journal. Keep the last durable phase and all
// live/bak/staging artifacts; boot recoverAll owns mutation.
//
// Rationale: restoring then failing to clear LiveSwitched leaves reboot to
// CommitNew registry metadata onto already-restored old files.

enum class InstallFailBranch : int {
  PromoteFailed = 0,
  LiveSwitchedJournalFailed = 1,
  RegistrySaveFailed = 2,
};

struct InstallFailSnapshot {
  bool bakExists = false;
  bool liveExists = false;
  bool stagingExists = false;
  bool hadPriorInstall = false;
  // Post-restore observation (for sequential regression of the unsafe path).
  bool restoreSucceeded = false;
  bool bakExistsAfterRestore = false;
  bool journalRemoveSucceeded = false;
  bool registryMatchesNew = false;
};

// Fail-closed: immediate install failure paths never clear the journal.
inline bool mayClearJournalOnInstallFail(InstallFailBranch /*branch*/, const InstallFailSnapshot& /*s*/) {
  return false;
}

// Fail-closed: immediate paths never call restore/drop.
inline bool mustAttemptSafeRestore(InstallFailBranch /*branch*/, bool /*bakExists*/) {
  return false;
}

// After an unsafe sequence (restore consumed bak, journal remove failed), boot must
// not CommitNew over old files. Shared with decideRecovery LiveSwitched rule.
inline bool mayCommitNewOnLiveSwitched(bool hadPriorInstall, bool bakExists, bool registryMatchesNew) {
  if (registryMatchesNew) return false;  // already durable — cleanup only
  if (hadPriorInstall && !bakExists) return false;
  return true;
}

// Sequential unsafe path → boot action (must not be CommitNewRegistryThenCleanup).
inline RecoveryAction bootAfterUnsafeImmediateRollback(const JournalRecord& rec, bool liveExists,
                                                       bool bakAfterRestore, bool registryMatchesNew) {
  FsSnapshot fs;
  fs.liveExists = liveExists;
  fs.bakExists = bakAfterRestore;
  fs.journalPresent = true;
  fs.registryMatchesNew = registryMatchesNew;
  return decideRecovery(rec, fs);
}

}  // namespace M4xInstallTxn
