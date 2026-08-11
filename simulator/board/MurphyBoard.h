#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "board/MurphyM4Spec.h"
#include "core/SimKernel.h"
#include "hardware/SimBattery.h"
#include "hardware/SimFrontlight.h"
#include "hardware/SimFt6x36.h"
#include "hardware/SimGpio.h"
#include "hardware/SimI2cBus.h"
#include "hardware/SimInput.h"
#include "hardware/SimPower.h"
#include "hardware/SimSdmmc.h"
#include "hardware/SimSleepPower.h"
#include "hardware/SimSpiBus.h"
#include "hardware/SimSsd1677Controller.h"

namespace m4sim {

class MurphyBoard {
 public:
  MurphyBoard(SimScheduler* sched, SimTrace* trace)
      : sched_(sched), trace_(trace),
        sdPower_(&gpio_, m4board::MurphyM4Spec::kSdPower,
                 m4board::MurphyM4Spec::kSdPowerActiveHigh, "sd"),
        touchPower_(&gpio_, m4board::MurphyM4Spec::kTouchPower,
                    m4board::MurphyM4Spec::kTouchPowerActiveHigh, "touch"),
        displaySpi_(m4board::MurphyM4Spec::kEpdSclk, m4board::MurphyM4Spec::kEpdMosi),
        epd_(sched, trace),
        i2c_(m4board::MurphyM4Spec::kI2cSda, m4board::MurphyM4Spec::kI2cScl, 400000),
        aht20Regs_(256),
        sdmmc_(sched, trace,
               SimSdmmc::Config{m4board::MurphyM4Spec::kSdClk,
                                m4board::MurphyM4Spec::kSdCmd,
                                m4board::MurphyM4Spec::kSdD0,
                                m4board::MurphyM4Spec::kSdD1,
                                m4board::MurphyM4Spec::kSdD2,
                                m4board::MurphyM4Spec::kSdD3,
                                m4board::MurphyM4Spec::kSdBusWidth, 2},
               [this] { return sdPower_.powered(); }),
        battery_(m4board::MurphyM4Spec::kBatteryDividerMultiplier),
        frontlight_(m4board::MurphyM4Spec::kFrontlightPwmHz,
                    m4board::MurphyM4Spec::kFrontlightResolutionBits, true),
        sleep_(&gpio_, &sdPower_, &touchPower_) {
    claimWiring();
    displaySpi_.attach(m4board::MurphyM4Spec::kEpdCs, "ssd1677", &epd_, 20000000);

    i2c_.attach(m4board::MurphyM4Spec::kTouchAddress, "ft6x36-touch", &touchController_,
                [this] { return touchPower_.powered(); }, true);
    i2c_.attach(m4board::MurphyM4Spec::kAht20Address, "aht20", &aht20Regs_,
                [] { return true; }, m4board::MurphyM4Spec::kAht20FirmwareEnabled);

    sdPower_.setPowered(false);
    touchPower_.setPowered(false);
    gpio_.driveExternal(m4board::MurphyM4Spec::kEpdBusy, false);
  }

  SimGpio& gpio() { return gpio_; }
  SimSpiBus& displaySpi() { return displaySpi_; }
  SimSsd1677Controller& epd() { return epd_; }
  SimI2cBus& i2c() { return i2c_; }
  SimFt6x36Device& touchController() { return touchController_; }
  SimSdmmc& sdmmc() { return sdmmc_; }
  SimPowerGate& sdPower() { return sdPower_; }
  SimPowerGate& touchPower() { return touchPower_; }
  SimBattery& battery() { return battery_; }
  SimFrontlight& frontlight() { return frontlight_; }
  SimInput& input() { return input_; }
  SimSleepPower& sleepPower() { return sleep_; }

  void syncSignals() {
    gpio_.driveExternal(m4board::MurphyM4Spec::kEpdBusy, epd_.busy());
    sdmmc_.notifyPowerChanged();
  }

  bool beginDisplayTransaction(uint32_t hz = m4board::MurphyM4Spec::kDisplaySpiHz) {
    return displaySpi_.begin(m4board::MurphyM4Spec::kEpdCs, hz);
  }
  bool endDisplayTransaction() { return displaySpi_.end(); }

  bool selfCheck(std::vector<std::string>& errors) const {
    errors = gpio_.violations();
    for (const auto& e : displaySpi_.errors()) errors.push_back(e);
    for (const auto& e : i2c_.errors()) errors.push_back(e);
    for (const auto& e : epd_.errors()) errors.push_back(e);
    for (const auto& e : sleep_.errors()) errors.push_back(e);
    return errors.empty();
  }

 private:
  void claimWiring() {
    auto out = [this](int pin, const char* owner) {
      gpio_.claim(pin, owner);
      gpio_.configure(pin, GpioMode::Output);
    };
    auto in = [this](int pin, const char* owner, GpioPull pull) {
      gpio_.claim(pin, owner);
      gpio_.configure(pin, GpioMode::Input, pull);
    };
    auto shared = [this](int pin, const char* owner) {
      gpio_.claim(pin, owner, true);
      gpio_.configure(pin, GpioMode::OpenDrain, GpioPull::Up);
      gpio_.write(pin, true);
    };

    out(m4board::MurphyM4Spec::kEpdSclk, "epd.sclk");
    out(m4board::MurphyM4Spec::kEpdMosi, "epd.mosi");
    out(m4board::MurphyM4Spec::kEpdCs, "epd.cs");
    out(m4board::MurphyM4Spec::kEpdDc, "epd.dc");
    out(m4board::MurphyM4Spec::kEpdReset, "epd.reset");
    in(m4board::MurphyM4Spec::kEpdBusy, "epd.busy", GpioPull::Down);

    in(m4board::MurphyM4Spec::kKeyUp, "key.up", GpioPull::Up);
    in(m4board::MurphyM4Spec::kKeyDown, "key.down", GpioPull::Up);
    in(m4board::MurphyM4Spec::kKeyLockPower, "key.lock_power", GpioPull::Up);
    in(m4board::MurphyM4Spec::kBatteryAdc, "battery.adc", GpioPull::None);

    shared(m4board::MurphyM4Spec::kI2cSda, "i2c0.sda");
    shared(m4board::MurphyM4Spec::kI2cScl, "i2c0.scl");
    in(m4board::MurphyM4Spec::kTouchIrq, "touch.irq", GpioPull::Up);

    out(m4board::MurphyM4Spec::kBuzzer, "buzzer");
    out(m4board::MurphyM4Spec::kFrontlightWarm, "frontlight.warm");
    out(m4board::MurphyM4Spec::kFrontlightCool, "frontlight.cool");

    out(m4board::MurphyM4Spec::kSdClk, "sdmmc.clk");
    shared(m4board::MurphyM4Spec::kSdCmd, "sdmmc.cmd");
    shared(m4board::MurphyM4Spec::kSdD0, "sdmmc.d0");
    shared(m4board::MurphyM4Spec::kSdD1, "sdmmc.d1");
    shared(m4board::MurphyM4Spec::kSdD2, "sdmmc.d2");
    shared(m4board::MurphyM4Spec::kSdD3, "sdmmc.d3");
  }

  SimScheduler* sched_ = nullptr;
  SimTrace* trace_ = nullptr;
  SimGpio gpio_;
  SimPowerGate sdPower_;
  SimPowerGate touchPower_;
  SimSpiBus displaySpi_;
  SimSsd1677Controller epd_;
  SimI2cBus i2c_;
  SimFt6x36Device touchController_;
  SimRegisterI2cDevice aht20Regs_;
  SimSdmmc sdmmc_;
  SimBattery battery_;
  SimFrontlight frontlight_;
  SimInput input_;
  SimSleepPower sleep_;
};

}  // namespace m4sim
