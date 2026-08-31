#include "ui/scene/GfxSceneRenderer.h"

// Explicit template instantiation for production GfxRenderer is handled
// via header-only templated render. This translation unit ensures the
// package compiles as a standalone object and can be extended for
// non-template helpers if needed.
namespace UiScene {
// No out-of-line definitions required; GfxSceneRenderer is header-only.
} // namespace UiScene
