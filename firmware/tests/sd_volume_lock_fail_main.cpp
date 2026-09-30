// Production FreeRTOS branch with an owner-aware stub. Rejects the first
// create, then checks nesting and release. Not a device or FAT mount.
#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>

#include "M4SdVolumeGuard.h"

namespace {
int gReject = 1;
int gDeviceReads = 0;

int rejectCreate() { return gReject; }
}  // namespace

int main() {
  m4SdVolumeLockSetCreateHook(rejectCreate);
  const auto started = std::chrono::steady_clock::now();
  assert(!m4SdVolumeLockPrepare());
  {
    M4SdVolumeGuard skipped;
  }
  assert(m4SdVolumeLockDepth() == 0);
  assert(gDeviceReads == 0);
  assert(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));

  gReject = 0;
  assert(m4SdVolumeLockPrepare());
  assert(m4SdVolumeLockPrepare());
  {
    M4SdVolumeGuard outer;
    assert(m4SdVolumeLockDepth() == 1);
    {
      M4SdVolumeGuard inner;
      assert(m4SdVolumeLockDepth() == 2);
      // goto fail leaves the inner scope; the owner releases on the way out.
      goto fail;
    }
  fail:
    assert(m4SdVolumeLockDepth() == 1);
    std::atomic<bool> entered{false};
    std::thread opponent([&] {
      assert(!m4SdVolumeLockTry());
      entered = true;
    });
    opponent.join();
    assert(entered);
  }
  assert(m4SdVolumeLockDepth() == 0);
  assert(m4SdVolumeLockTry());
  m4SdVolumeLockGive();
  assert(m4SdVolumeLockDepth() == 0);
  assert(gDeviceReads == 0);
  return 0;
}
