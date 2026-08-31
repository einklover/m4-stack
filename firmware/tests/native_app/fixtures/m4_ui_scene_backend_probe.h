#pragma once

#include "ui/pages/HomeSceneModel.h"

namespace M4UiSceneBackendProbe {

enum class Mode : uint8_t { NeverReturns, Error, Empty, StaleAfterRefreshFailure };

struct Probe {
  Mode mode;
  uint32_t calls = 0;

  bool run(HomeScene::HomeSceneModel& model) {
    ++calls;
    switch (mode) {
      case Mode::NeverReturns:
        return false;
      case Mode::Error:
        return model.publishError(7);
      case Mode::Empty:
        return model.publishEmpty();
      case Mode::StaleAfterRefreshFailure:
        return model.publishStale();
    }
    return false;
  }
};

}  // namespace M4UiSceneBackendProbe
