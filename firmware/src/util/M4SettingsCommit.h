#pragma once

namespace M4SettingsCommit {
// The caller has fully written and synced tmp. Every failed step leaves a
// complete primary or backup; the old primary is never truncated in place.
template <typename Files>
bool commit(Files& files, const char* tmp, const char* primary, const char* backup,
            bool& primaryInvalid) {
  if (primaryInvalid && files.exists(primary) && !files.remove(primary)) return false;
  if (files.exists(primary)) {
    if (files.exists(backup) && !files.remove(backup)) return false;
    if (!files.rename(primary, backup)) return false;
  }
  if (!files.rename(tmp, primary)) {
    if (files.exists(backup) && !files.exists(primary)) files.rename(backup, primary);
    return false;
  }
  primaryInvalid = false;
  return true;
}
}  // namespace M4SettingsCommit
