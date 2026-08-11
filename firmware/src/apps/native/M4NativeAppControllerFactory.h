#pragma once

#include "apps/M4xRegistry.h"
#include "apps/native/M4NativeUiController.h"

#include <memory>

namespace M4NativeAppControllers {

// Built-in native controller factory. Provider adapters register here; XML
// never names C++ classes directly. Unknown providers still get a safe base
// controller so the page can render an explanatory state instead of executing
// package code.
std::unique_ptr<M4NativeUi::Controller> create(const M4xInstalledApp& app);

}  // namespace M4NativeAppControllers
