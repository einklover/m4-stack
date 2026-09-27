#pragma once

// Held for one SdFat function that uses the shared FsCache pointer, including
// the caller that keeps the pointer after a helper returns. Recursive:
// FatFile::read takes it and then calls fatGet, which takes it again.
// The same task must construct and destroy each guard. Do not hold it across
// network, UI, delay, or a Print callback — those sit outside the SdFat call.
class M4SdVolumeGuard {
 public:
  M4SdVolumeGuard();
  ~M4SdVolumeGuard();
  M4SdVolumeGuard(const M4SdVolumeGuard&) = delete;
  M4SdVolumeGuard& operator=(const M4SdVolumeGuard&) = delete;

 private:
  bool held_ = false;
};

// Creates the recursive mutex once. False means the cache must not be touched.
// Production uses static storage, so this does not allocate on later I/O.
bool m4SdVolumeLockPrepare();

// Test seam. Null is production. A hook that returns non-zero fails the
// one-time create; Prepare then returns false without spinning.
void m4SdVolumeLockSetCreateHook(int (*hook)());

// Zero-timeout attempt. The caller that receives true must call Give.
// Production paths use the guard. Tests use Try/Give to show another task
// cannot enter, and that scope exit releases the mutex.
bool m4SdVolumeLockTry();
void m4SdVolumeLockGive();
int m4SdVolumeLockDepth();
