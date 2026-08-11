#pragma once

// Pure decisions + production-shared replace/reconcile algorithm for app-data
// state files after interrupted replaceFile.
// Empty content is a valid durable state (do not treat length==0 as missing).
// Incomplete FAT copy fallback leaves live(partial)+bak(old)+tmp(new) — that
// triple must NOT be treated as a completed live file.

#include <map>
#include <string>

namespace M4xStateFile {

struct Snap {
  bool liveExists = false;  // path exists (including empty file)
  bool bakExists = false;   // .bak exists (including empty)
  bool tmpExists = false;
};

enum class Action {
  UseLiveDropTmp,  // durable live present (empty OK) — drop incomplete tmp only
  RestoreBak,      // incomplete txn or missing live — promote bak → live
  DropTmpOnly,     // incomplete write debris only
  None,            // nothing present
};

// Core rule: live+bak+tmp together means the tmp→live copy was interrupted.
// Prefer known-good bak over a possibly partial live; empty-only live (no bak/tmp)
// remains valid.
inline Action decide(const Snap& s) {
  if (s.liveExists && s.bakExists && s.tmpExists) {
    return Action::RestoreBak;
  }
  if (s.liveExists) return Action::UseLiveDropTmp;
  if (s.bakExists) return Action::RestoreBak;
  if (s.tmpExists) return Action::DropTmpOnly;
  return Action::None;
}

// After a restore attempt: may drop bak only when live is proven present.
inline bool mayDropBakAfterRestore(bool liveExistsNow, bool restoreIoOk) {
  return restoreIoOk && liveExistsNow;
}

inline bool mayForgetTmp(bool removeTmpOk) { return removeTmpOk; }

// replaceFile transitions (for failure-injection tests).
enum class ReplaceStep {
  WriteTmp = 0,
  LiveToBak = 1,
  TmpToLive = 2,  // may be rename or copy; copy can leave partial live
  DropBak = 3,
  Done = 4,
};

// After crash at completed step: what should reconcile do?
inline Action afterReplaceCrash(ReplaceStep completed, bool liveExists, bool bakExists, bool tmpExists) {
  Snap s{liveExists, bakExists, tmpExists};
  switch (completed) {
    case ReplaceStep::WriteTmp:
      return decide(s);
    case ReplaceStep::LiveToBak:
      return decide(s);
    case ReplaceStep::TmpToLive:
      // Successful TmpToLive would leave live+bak without tmp (tmp consumed).
      // If tmp still exists with live+bak → incomplete copy (RestoreBak).
      return decide(s);
    case ReplaceStep::DropBak:
    case ReplaceStep::Done:
      return decide(s);
    default:
      return Action::None;
  }
}

// ---- Host-test policy mirror (NOT device l_fs_replaceFile / SdMan I/O) ----
// Exercises the same decide/reconcile rules with injectable failures. Do not
// describe these tests as executing production filesystem code.

struct MemFs {
  std::map<std::string, std::string> files;  // path -> body (empty string is valid file)

  // Fail the Nth mutating op (0-based). -1 = never.
  int failMutateAt = -1;
  int mutateCount = 0;
  // When writing live via copy fallback, stop after this many bytes (-1 = full).
  int shortWriteLiveBytes = -1;

  bool exists(const std::string& p) const { return files.find(p) != files.end(); }

  bool readExact(const std::string& p, std::string& out) const {
    auto it = files.find(p);
    if (it == files.end()) return false;
    out = it->second;
    return true;
  }

  bool writeExact(const std::string& p, const std::string& body) {
    if (mutateCount++ == failMutateAt) return false;
    files[p] = body;
    return true;
  }

  // Partial write to live during tmp→live copy fallback (simulates power loss).
  bool writePartial(const std::string& p, const std::string& body, int maxBytes) {
    if (mutateCount++ == failMutateAt) return false;
    if (maxBytes < 0 || maxBytes >= static_cast<int>(body.size())) {
      files[p] = body;
      return true;
    }
    files[p] = body.substr(0, static_cast<size_t>(maxBytes));
    return false;  // incomplete
  }

  bool remove(const std::string& p) {
    if (mutateCount++ == failMutateAt) return false;
    files.erase(p);
    return true;
  }

  bool rename(const std::string& src, const std::string& dst) {
    if (mutateCount++ == failMutateAt) return false;
    auto it = files.find(src);
    if (it == files.end()) return false;
    files[dst] = it->second;
    files.erase(it);
    return true;
  }
};

// Production replaceFile algorithm (mirror of l_fs_replaceFile).
// forceCopyTmpToLive: exercise FAT copy path instead of rename.
// Returns true only when replacement is fully durable.
inline bool replaceFileProd(MemFs& fs, const std::string& path, const std::string& body,
                            bool forceCopyTmpToLive = false) {
  const std::string tmp = path + ".tmp";
  const std::string bak = path + ".bak";

  // reconcile first
  {
    Snap s{fs.exists(path), fs.exists(bak), fs.exists(tmp)};
    Action a = decide(s);
    if (a == Action::RestoreBak) {
      // Remove partial live only after bak readable.
      std::string bakBody;
      if (!fs.readExact(bak, bakBody)) return false;
      if (fs.exists(path)) {
        if (!fs.remove(path)) return false;
      }
      if (!fs.writeExact(path, bakBody)) return false;
      if (fs.exists(bak) && !fs.remove(bak)) {
        /* retain bak */
      }
      if (fs.exists(tmp)) (void)fs.remove(tmp);
    } else if (a == Action::UseLiveDropTmp) {
      if (fs.exists(tmp)) (void)fs.remove(tmp);
    } else if (a == Action::DropTmpOnly) {
      if (fs.exists(tmp)) (void)fs.remove(tmp);
    }
  }

  if (!fs.writeExact(tmp, body)) {
    (void)fs.remove(tmp);
    return false;
  }

  // live → bak
  if (fs.exists(bak)) {
    if (!fs.remove(bak)) return false;
  }
  if (fs.exists(path)) {
    if (!fs.rename(path, bak)) {
      std::string old;
      if (!fs.readExact(path, old)) {
        (void)fs.remove(tmp);
        return false;
      }
      if (!fs.writeExact(bak, old)) {
        (void)fs.remove(tmp);
        return false;
      }
      if (!fs.remove(path)) {
        (void)fs.remove(tmp);
        return false;
      }
    }
  }

  // tmp → live
  if (!forceCopyTmpToLive && fs.rename(tmp, path)) {
    if (fs.exists(bak)) (void)fs.remove(bak);
    return true;
  }

  // Copy fallback (FAT): may be interrupted mid-write.
  std::string nb;
  if (!fs.readExact(tmp, nb)) {
    if (fs.exists(bak)) (void)fs.rename(bak, path);
    return false;
  }
  if (fs.shortWriteLiveBytes >= 0) {
    if (!fs.writePartial(path, nb, fs.shortWriteLiveBytes)) {
      // leave live partial + bak + tmp for reconcile
      return false;
    }
  } else {
    if (!fs.writeExact(path, nb)) {
      if (fs.exists(path)) (void)fs.remove(path);
      if (fs.exists(bak)) (void)fs.rename(bak, path);
      (void)fs.remove(tmp);
      return false;
    }
  }
  if (!fs.remove(tmp)) {
    // live complete but tmp remains — next reconcile UseLiveDropTmp if no bak, etc.
  }
  if (fs.exists(bak)) (void)fs.remove(bak);
  return true;
}

// Production reconcile algorithm (mirror of reconcileStateFile).
inline void reconcileProd(MemFs& fs, const std::string& path) {
  const std::string tmp = path + ".tmp";
  const std::string bak = path + ".bak";
  Snap s{fs.exists(path), fs.exists(bak), fs.exists(tmp)};
  Action a = decide(s);
  if (a == Action::UseLiveDropTmp) {
    if (fs.exists(tmp)) (void)fs.remove(tmp);
    return;
  }
  if (a == Action::RestoreBak) {
    std::string bakBody;
    if (!fs.readExact(bak, bakBody)) return;  // retain all
    // Replace partial live with bak content (do not destroy bak until live proven).
    if (fs.exists(path)) {
      if (!fs.remove(path)) return;
    }
    if (!fs.writeExact(path, bakBody)) return;
    // live proven (empty OK)
    if (fs.exists(bak)) (void)fs.remove(bak);
    if (fs.exists(tmp)) (void)fs.remove(tmp);
    return;
  }
  if (a == Action::DropTmpOnly) {
    if (fs.exists(tmp)) (void)fs.remove(tmp);
  }
}

}  // namespace M4xStateFile
