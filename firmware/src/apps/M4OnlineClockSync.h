#pragma once

namespace M4OnlineClockSync {

// If system time is outside M4ClockPolicy bounds and Wi-Fi/ETH is already
// up, run one bounded SNTP poll (does not start or stop the radio, does not
// hang the UI beyond kTimeoutMs). No-ops when the clock is already sane or
// after the first attempt this boot. Writes RTC on success when present.
void ensureOnce();

}  // namespace M4OnlineClockSync
