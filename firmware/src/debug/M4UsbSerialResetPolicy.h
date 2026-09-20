#pragma once

// ESP32-S3 USB Serial/JTAG (CDC) asserts DTR on host open() and that can
// raise rst:0x15 USB_UART_CHIP_RESET. Setting these RTC bits *before*
// Serial.begin() is the production workaround — not a QEMU-only shim.
// Host-testable: constants and apply-gate compile without IDF headers.

#include <cstdint>

#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ARDUINO_ARCH_ESP32S3)
#include "soc/rtc_cntl_reg.h"
#endif

namespace M4UsbSerialResetPolicy {

// RTC_CNTL_USB_CONF_REG bits (ESP32-S3). Named here so host tests need no IDF.
inline constexpr uint32_t kIoMuxResetDisableBit = 1u << 18;
inline constexpr uint32_t kUsbResetDisableBit = 1u << 17;
inline constexpr uint32_t kUsbSerialResetDisableMask = kIoMuxResetDisableBit | kUsbResetDisableBit;

inline constexpr bool kApplyOnThisBuild =
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ARDUINO_ARCH_ESP32S3)
    true
#else
    false
#endif
    ;

#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ARDUINO_ARCH_ESP32S3)
static_assert(RTC_CNTL_IO_MUX_RESET_DISABLE == kIoMuxResetDisableBit, "IDF IO_MUX_RESET_DISABLE bit");
static_assert(RTC_CNTL_USB_RESET_DISABLE == kUsbResetDisableBit, "IDF USB_RESET_DISABLE bit");
#endif

inline void applyBeforeSerialBegin() {
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ARDUINO_ARCH_ESP32S3)
  REG_SET_BIT(RTC_CNTL_USB_CONF_REG, RTC_CNTL_IO_MUX_RESET_DISABLE | RTC_CNTL_USB_RESET_DISABLE);
#endif
}

}  // namespace M4UsbSerialResetPolicy
