#pragma once

// Batch-only skip policy for /apps_inbox .m4x files.
// Installer still overwrites on equal versionCode; this layer deletes
// same-or-older packages so a folder of mixed zips can be processed unattended.

enum class M4xInboxBatchAction { Install, SkipDelete, Keep };

inline M4xInboxBatchAction m4xInboxBatchDecide(bool probeOk, bool hasInstalled, int incomingVersionCode,
                                               int installedVersionCode) {
  if (!probeOk) return M4xInboxBatchAction::Keep;
  if (hasInstalled && incomingVersionCode <= installedVersionCode) return M4xInboxBatchAction::SkipDelete;
  return M4xInboxBatchAction::Install;
}
