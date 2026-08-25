#pragma once

#include <cstddef>
#include <string>

namespace M4NativeCatalogPolicy {

// Native catalog HTTPS/JSON/SD call chains are deep enough that the old 24 KiB
// external-RAM stack can overflow. Match the already-stable native provider
// workers.
inline constexpr size_t kTaskStackBytes = 72u * 1024u;

// Fanqie long catalogs can contain thousands of chapters. Do not duplicate the
// whole TSV in PSRAM after the first-window open; stream it to the buffered SD
// sink instead. Other providers keep the established PSRAM-first path.
inline bool preferPsramAssembly(const std::string& providerId) {
  return providerId != "fanqie";
}

}  // namespace M4NativeCatalogPolicy
