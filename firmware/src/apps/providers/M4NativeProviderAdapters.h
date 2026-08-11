#pragma once

#include "apps/providers/M4NativeProvider.h"

#include <memory>
#include <string>

namespace M4NativeProviderAdapters {

std::unique_ptr<M4NativeProvider::Adapter> create(const std::string& providerId);

}  // namespace M4NativeProviderAdapters
