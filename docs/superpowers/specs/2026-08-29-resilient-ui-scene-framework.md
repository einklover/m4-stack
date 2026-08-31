# Resilient M4 UI Scene Framework

## Status and scope

This is the approved architectural specification for replacing the Home-only
scene runtime with a reusable M4 UI Scene Framework. It covers the generic
scene/compiler/runtime boundary, immutable asynchronous UI state, the Home
migration, a Settings mock proof, and failure-injection verification.

This specification does not migrate every real Settings or Plugin surface. It
does not change reader pagination, provider protocols, plugin runtimes, Lua,
network APIs, SD formats, partitions, flashing, or the existing activity
navigation contract. Those systems become backend producers or action targets
only through adapters added in later work.

## Current boundary

The dirty worktree already contains the first Home Scene implementation:

- `firmware/tools/compile_home_theme.py` compiles the M4TH package and its
  ordered scene nodes.
- `firmware/src/util/HomeSceneRuntime.h` validates M4TH, iterates SCENE
  commands in serialized order, and exposes binding/action metadata.
- `firmware/src/util/HomeMofeiTemplateOverlay.h` streams the M4TH
  `ASSET_DATA` bitmap as black ink.
- `firmware/src/activities/home/HomeActivity.cpp` owns the Home task, input,
  rendering, recent-book/cover loading, activity callbacks, and theme-specific
  composition in one class.
- `SettingsActivity` owns a separate display task and mutex, builds setting
  vectors in `onEnter()`, reads settings directly while rendering, and invokes
  subactivities or saves settings from input handling.
- `NativeAppActivity` and `M4NativeUi::Controller` already have a useful
  `revision()`/`pollAsync()` direction, but render still resolves controller
  values and rows, and some controller paths remain coupled to provider state.
- App/file/plugin surfaces include `AppListActivity`, `AppRuntimeActivity`,
  `NativeAppActivity`, `M4NativeUi`, and `M4FileRowSource`; they are future
  adapters, not required migrations in this change.

The framework must absorb the useful ordered-scene and asynchronous-revision
ideas without preserving Home-specific names or allowing a render callback to
reach a provider, plugin, network, SD card, Lua VM, or blocking lock.

## Goals

1. Make one bounded scene core usable by Home, Settings, Plugins, File Manager,
   and future pages.
2. Make UI response the hard priority: render, input, focus, back/home, and
   navigation must remain responsive while backend work is slow, failed, or
   permanently stalled.
3. Make backend output an immutable, stable snapshot. UI renders only the
   latest complete snapshot it copied or acquired.
4. Keep scene JSON node order as exact paint order.
5. Keep numeric binding and action IDs in the runtime. Page adapters own the
   meaning of those IDs.
6. Preserve the current Home output and action behavior during the first
   migration, including the optional Mofei bitmap/template path.
7. Keep the device hot path bounded-memory and free of heavyweight STL
   containers and allocations.

## Non-goals

- No synchronous backend fallback from the renderer.
- No provider/plugin/network/SD/Lua calls from `render()`, hit testing,
  focus movement, or system navigation.
- No dynamic expression evaluator, scripting language, CSS engine, or arbitrary
  XML/JSON execution in the device runtime.
- No requirement that every backend result be available before the first paint.
- No real Settings or Plugin screen migration beyond the mock scene proof.
- No claim of hardware behavior from host or simulator tests.

## Architectural model

```text
scene JSON / M4TH pack
          |
          v
M4UiSceneRuntime: bounded validation + ordered node iteration
          |
          v
M4UiSceneExecutor <---- latest UiStateSnapshot ---- UiStateStore <---- backend task
          |                       ^                                      |
          |                       |                                      |
          +--> renderer           +-- page adapter maps IDs               |
          +--> hit regions            to values/actions                   |
          +--> nonblocking action queue --------------------------> provider/plugin/SD/Lua/network

system input/navigation (back/home/focus) stays on the UI side and never waits
for the right-hand side.
```

There are three independent flows:

1. `renderFrame()` obtains one stable latest snapshot, walks the scene in
   serialized order, and paints from only that snapshot plus immutable scene
   data and renderer-owned resources.
2. `handleInput()` performs hit testing and system navigation immediately. A
   page action becomes a small numeric `UiAction` placed into a bounded queue;
   it does not execute the action.
3. A backend task consumes queued work, performs provider/plugin/network/SD/Lua
   operations, builds a new complete snapshot off to the side, and publishes
   it atomically. Publication is the only handoff to the UI.

## Canonical interfaces

The following interfaces are the contract to implement. Names and field widths
are intentional because they define the device memory and ABI boundary.

### `M4UiSceneRuntime`

Canonical location: `firmware/src/util/M4UiSceneRuntime.h`.

`HomeSceneRuntime.h` becomes a compatibility header that includes the canonical
header and aliases the old namespace/constants while existing dirty Home tests
are migrated. It must not remain the implementation owner.

```cpp
namespace M4UiScene {
using BindingId = uint8_t;
using ActionId = uint8_t;
using SceneNodeId = uint8_t;

struct SceneCommand {
  SceneNodeId type;
  uint8_t flags;
  uint16_t payloadLen;
  const uint8_t* payload;   // immutable M4TH/PROGMEM bytes
  uint32_t offset;
};

bool validatePackage(const uint8_t* data, size_t len);
bool findSection(const uint8_t* data, size_t len, uint32_t type, SectionInfo* out);
bool parseSceneHeader(const uint8_t* data, size_t len, SceneHeader* out);

template <typename Visitor>
bool forEachCommand(const uint8_t* data, size_t len, Visitor&& visitor);
}
```

The runtime keeps the current M4TH v1 bounds, 480x800 contract, four-byte
padding checks, repeat limits, and no-allocation parsing. It may add explicit
numeric binding/action helpers, but it must not resolve their meaning or call a
page/backend object.

### `UiStateSnapshot`

Canonical location: `firmware/src/util/M4UiStateSnapshot.h`.

The snapshot is page-agnostic. It contains values indexed by numeric IDs and
bounded lists/items; it contains no provider class, callback, `std::string`,
filesystem handle, network object, Lua value, or owned heap pointer.

```cpp
namespace M4Ui {
constexpr size_t kMaxBindings = 64;
constexpr size_t kMaxLists = 8;
constexpr size_t kMaxItems = 32;
constexpr size_t kMaxTextBytes = 1536;
constexpr size_t kMaxAssets = 8;

enum class UiLoadState : uint8_t { Loading, Ready, Empty, Stale, Error };
enum class UiValueKind : uint8_t { Missing, Bool, Int, Text, Asset };

struct UiText { uint16_t offset; uint16_t length; };
struct UiAssetRef {
  uint16_t assetId;
  const uint8_t* pixels;   // immutable for the publication lifetime
  uint16_t width;
  uint16_t height;
  uint16_t stride;
  uint8_t format;          // 1bpp only in the first implementation
};
struct UiBindingValue {
  BindingId id;
  UiValueKind kind;
  uint8_t flags;
  int32_t number;
  UiText text;
  uint16_t assetIndex;
};
struct UiListItem {
  uint16_t listId;
  uint8_t index;
  uint8_t flags;
  uint16_t key;
  UiText title;
  UiText subtitle;
  UiText value;
  uint16_t assetIndex;
};
struct UiStateSnapshot {
  uint32_t revision;
  uint32_t sourceEpoch;
  UiLoadState state;
  uint8_t errorCode;
  uint8_t stale;
  uint16_t bindingCount;
  uint16_t listItemCount;
  uint16_t textUsed;
  uint16_t assetCount;
  UiBindingValue bindings[kMaxBindings];
  UiListItem items[kMaxItems];
  UiAssetRef assets[kMaxAssets];
  char text[kMaxTextBytes];
};
}
```

The implementation may use a smaller packed representation if the same
semantics and limits are preserved. `UiLoadState::Loading`, `Empty`, `Stale`,
and `Error` are content states, not activity states. Every page must still
paint system controls and accept system navigation in all four states.

Asset pointers are valid only for the immutable publication lifetime. The
backend owns a fixed asset arena or a reference-count-free retired buffer
scheme that does not overwrite an asset while a UI frame can read it. If an
asset cannot be published safely, the snapshot carries no asset and the
executor paints a placeholder; it never opens SD during rendering.

### `UiStateStore`

Canonical location: `firmware/src/util/M4UiStateStore.h` and
`firmware/src/util/M4UiStateStore.cpp`.

```cpp
class UiStateStore final {
 public:
  UiStateStore();
  bool publish(const UiStateSnapshot& next);       // bounded copy + release
  bool copyLatest(UiStateSnapshot& out) const;     // bounded read + acquire
  uint32_t latestRevision() const;
  bool hasPublishedSnapshot() const;
};
```

The first implementation uses two fixed snapshot buffers and a short
publication/read critical section. On ESP32 it uses a `portMUX_TYPE` critical
section (or an equally bounded FreeRTOS primitive) only around the fixed
`memcpy` and index swap. On host it uses an atomic index plus the same bounded
copy discipline. No `std::mutex`, `std::vector`, or allocation is permitted in
the UI or publication path. `copyLatest()` returns a complete snapshot or
`false`; it never returns a partially written state.

The UI copies once at the start of a frame and holds the copy as a local frame
snapshot. A later backend publication affects the next frame only.

### `M4UiSceneExecutor`

Canonical location: `firmware/src/util/M4UiSceneExecutor.h` and
`firmware/src/util/M4UiSceneExecutor.cpp`.

```cpp
class PageAdapter {
 public:
  virtual ~PageAdapter() = default;
  virtual bool value(BindingId id, const UiStateSnapshot& snapshot,
                     const ItemContext& item, ResolvedValue& out) const = 0;
  virtual bool actionRect(ActionId id, const UiStateSnapshot& snapshot,
                          const ItemContext& item, Rect& out) const = 0;
};

class UiActionSink {
 public:
  virtual ~UiActionSink() = default;
  virtual bool enqueue(const UiAction& action) = 0; // must not execute it
};

class M4UiSceneExecutor final {
 public:
  bool render(const uint8_t* scene, size_t sceneLen,
              const UiStateSnapshot& snapshot, const PageAdapter& adapter,
              GfxRenderer& renderer) const;
  bool hitTest(const uint8_t* scene, size_t sceneLen,
               const UiStateSnapshot& snapshot, const PageAdapter& adapter,
               int16_t x, int16_t y, UiAction* out) const;
};
```

The concrete names for `ResolvedValue`, `ItemContext`, and `UiAction` are
defined in the same header with fixed-size fields. `UiAction` contains only an
`ActionId`, scene/page ID, item index/key, and a bounded argument; it contains
no callback or string ownership.

The executor handles clear, line, rect, round-rect, text, cover/image,
progress, icon, battery, group, repeat, visibility, and action metadata. The
existing template/bitmap path is one optional ordered node (`bitmap`) and is
painted exactly where its node appears in the scene. It is not a special
pre-render/post-render overlay branch.

Unknown or unavailable content bindings are rendered as absent, loading,
empty, stale, or error content according to the snapshot. The executor still
renders system chrome and returns hit results for system controls.

### `M4UiSceneController`

Canonical location: `firmware/src/util/M4UiSceneController.h` and
`firmware/src/util/M4UiSceneController.cpp`.

This small controller owns frame scheduling, the local copied snapshot,
focus/index state, system back/home handling, and the action queue boundary.
It does not own a provider and does not know Home/Settings/Plugin semantics.
Its public operations are:

```cpp
void requestFrame();
void onSystemInput(const MappedInputEvent& event);  // immediate, bounded
void onPageInput(const MappedInputEvent& event);    // hit test + enqueue only
bool renderIfRequested(GfxRenderer& renderer);     // snapshot copy + paint
```

Navigation callbacks may change the active activity only through the existing
activity manager. A page action is queued for the page backend. Back, home,
focus movement, footer hit testing, and frame scheduling do not depend on queue
capacity or backend progress.

## Page adapter rules

Each page provides numeric maps, not runtime conditionals:

- `HomePageAdapter` maps current/recent/app/battery/Wi-Fi binding IDs to the
  snapshot and maps Home action IDs to queued activity actions.
- `SettingsPageAdapter` maps category/row/value binding IDs and action IDs;
  the first implementation is a test/mock adapter and does not replace the
  real `SettingsActivity`.
- Future `PluginPageAdapter`, `FileManagerPageAdapter`, and provider adapters
  translate their own IDs and build snapshots in backend tasks.

The same scene pack may therefore render different pages without adding page
names to `M4UiSceneExecutor`. Page adapters may read the snapshot and perform
pure mapping only. Their `value()` and hit-test methods must not perform I/O,
lock on backend state, allocate, or call action callbacks.

## Backend and failure semantics

Each backend task follows this lifecycle:

1. Publish a complete `Loading` snapshot immediately on page entry.
2. Perform bounded, cancellable work outside the UI task.
3. Publish a complete `Ready` snapshot on success.
4. Publish `Empty` when the source is valid but has no content.
5. Publish `Stale` with the last complete content when refresh fails but old
   content exists.
6. Publish `Error` with a small numeric error code when no usable content
   exists.

The backend must never hold the state-store publication section while reading
SD, waiting for a network response, running Lua, parsing a plugin response, or
decoding a large asset. A stalled backend simply stops publishing; the UI
continues to render the last snapshot and process system input.

Home-specific work currently done by `loadRecentBooks()`, `loadBookProgress()`,
`loadRecentCovers()`, registry lookup, and cover generation moves behind a Home
backend task. The Home UI task receives only fixed snapshot data and immutable
asset references. Activity callbacks are represented by numeric action IDs and
consumed outside renderer/input code.

## Memory and timing budget

- No dynamic STL container in the executor, state store, hit tester, or frame
  loop.
- No `std::string` in `UiStateSnapshot`, `UiAction`, or scene traversal.
- Maximum scene node, repeat, binding, item, text, and asset counts remain
  compile-time bounded.
- Publication and snapshot copy are fixed-size and bounded; they may be
  measured, but may not include backend work.
- A frame must be able to render from a snapshot even if the backend task is
  suspended indefinitely.
- Any larger bitmap is prepared by backend work into a bounded immutable asset
  arena or omitted with a placeholder.
- Production RAM/flash deltas must be recorded after the final build.

## Compatibility and migration constraints

1. Existing M4TH v1 packs remain readable.
2. `compile_home_theme.py` remains a compatibility entry point while generic
   compiler code is extracted to `compile_m4_ui_scene.py`.
3. The current generated Home headers remain valid during the extraction.
4. `HomeSceneRuntime` names remain available only as aliases during migration,
   then can be deprecated after all tests use `M4UiScene`.
5. Existing Home hit geometry and activity callback meanings remain covered by
   tests; the framework changes the timing/ownership boundary, not the user
   actions.

## Acceptance criteria

- A single generic runtime executes Home and the Settings mock scene.
- Scene node order is observable and unchanged by executor traversal.
- A bitmap/template node can appear before or after dynamic nodes and is
  painted in that exact order.
- Renderer and hit testing make no provider/plugin/network/SD/Lua calls.
- A stalled or failed backend leaves render, focus, back/home, and queued input
  responsive in host and simulator failure-injection tests.
- Home reaches loading first, then ready/stale/error without blocking first
  paint.
- The real Settings and Plugin surfaces are unchanged except for reusable
  framework interfaces added for future migration.
- `murphy_m4` compiles successfully and the simulator preview shows Home and
  the Settings mock scene at 480x800.
- Only the approved framework, Home migration, test, compiler/preview, and
  documentation files are changed by implementation.
