#pragma once

// Held for one SdFat function that uses the shared FsCache pointer.
// Recursive: FatFile::read takes it and then calls fatGet, which takes it again.
// The same task must construct and destroy each guard. Do not hold it across
// network, UI, or delay — those sit outside the SdFat call.
class M4SdVolumeGuard {
 public:
  M4SdVolumeGuard();
  ~M4SdVolumeGuard();
  M4SdVolumeGuard(const M4SdVolumeGuard&) = delete;
  M4SdVolumeGuard& operator=(const M4SdVolumeGuard&) = delete;
};

// Zero-timeout attempt. The caller that receives true must call Give.
// Production paths use the guard. Tests use Try/Give to show another task
// cannot enter, and that scope exit releases the mutex.
bool m4SdVolumeLockTry();
void m4SdVolumeLockGive();
int m4SdVolumeLockDepth();
