#include "apps/providers/M4NativeProviderAdapters.h"

namespace M4NativeProviderAdapters {

std::unique_ptr<M4NativeProvider::Adapter> createFanqieProvider();
std::unique_ptr<M4NativeProvider::Adapter> createJjwxcProvider();
std::unique_ptr<M4NativeProvider::Adapter> createWereadProvider();
std::unique_ptr<M4NativeProvider::Adapter> createLegadoProvider();

std::unique_ptr<M4NativeProvider::Adapter> create(const std::string& providerId) {
  if (providerId == "fanqie") return createFanqieProvider();
  if (providerId == "jjwxc") return createJjwxcProvider();
  if (providerId == "weread") return createWereadProvider();
  if (providerId == "legado") return createLegadoProvider();
  return {};
}

}  // namespace M4NativeProviderAdapters
