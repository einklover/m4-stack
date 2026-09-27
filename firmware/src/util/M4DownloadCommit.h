#pragma once
namespace M4DownloadCommit {
// tmp is a complete, synced, exclusively-created download. Backup must not
// exist: never delete an unrelated or interrupted recovery copy to make room.
template <typename Files>
bool commit(Files& files, const char* tmp, const char* path, const char* backup) {
  if (files.exists(backup)) return false;
  const bool hadOriginal = files.exists(path);
  if (hadOriginal && !files.rename(path, backup)) return false;
  if (!files.rename(tmp, path)) {
    if (hadOriginal) files.rename(backup, path); // Failed rollback leaves backup intact.
    return false;
  }
  if (hadOriginal) files.remove(backup); // Failed cleanup keeps the previous complete copy.
  return true;
}
}  // namespace M4DownloadCommit
