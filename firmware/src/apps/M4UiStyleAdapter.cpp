#include "apps/M4UiStyleAdapter.h"

#include <type_traits>

// Keep the retained scene style allocation-free. This translation unit also
// ensures the firmware toolchain continuously compiles the UITheme adapter.
static_assert(std::is_trivially_copyable<M4UiStyle::Theme>::value,
              "M4UiStyle::Theme must remain a small allocation-free value");
