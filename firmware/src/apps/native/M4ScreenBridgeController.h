#pragma once

#include "apps/M4xRegistry.h"
#include "apps/native/M4NativeUiController.h"

#include <cstdint>
#include <memory>

namespace M4NativeAppControllers {

std::unique_ptr<M4NativeUi::Controller> createScreenBridgeController(const M4xInstalledApp& app);

}  // namespace M4NativeAppControllers
