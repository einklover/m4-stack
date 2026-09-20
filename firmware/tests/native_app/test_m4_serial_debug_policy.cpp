#include "debug/M4SerialDebugPolicy.h"
#include "debug/M4UsbSerialResetPolicy.h"

#include <cstring>
#include <iostream>

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::cerr << "m4_serial_debug_policy FAIL: " #cond "\n"; \
      return 1; \
    } \
  } while (0)

int main() {
  using namespace M4SerialDebugPolicy;
  CHECK(std::strcmp(unauthorizedFrameError("req"), kUnauthorizedErrorKey) == 0);
  CHECK(std::strcmp(unauthorizedFrameError("chk"), "usb_debug_off") == 0);
  CHECK(unauthorizedFrameError("ok") == nullptr);
  CHECK(unauthorizedFrameError("err") == nullptr);
  CHECK(unauthorizedFrameError("prg") == nullptr);
  CHECK(unauthorizedFrameError("noise") == nullptr);
  CHECK(unauthorizedFrameError("") == nullptr);
  CHECK(unauthorizedFrameError(nullptr) == nullptr);
  CHECK(!opCanEnableAuthorization("ping"));
  CHECK(!opCanEnableAuthorization("install_begin"));

  CHECK(!M4UsbSerialResetPolicy::kApplyOnThisBuild);
  CHECK(M4UsbSerialResetPolicy::kUsbSerialResetDisableMask == ((1u << 18) | (1u << 17)));
  M4UsbSerialResetPolicy::applyBeforeSerialBegin();

  std::cout << "m4 serial debug policy tests passed\n";
  return 0;
}
