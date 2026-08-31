# M4 Home Theme Package — Design Specification

Status: APPROVED FOR IMPLEMENTATION  
Date: 2026-08-29  
Scope: Home page only (v1)

## 1. Goal

Replace the hard-coded Fengyan Home composition with a reusable, declarative theme-package system while preserving a safe built-in fallback. A theme author edits JSON + source images on a host computer; the device consumes a compact binary `.m4theme` pack. The first built-in theme is derived from:

`/Users/zhouxinlai/Downloads/72B74725-E0C3-4EF8-9179-E57F7E1BAD68.JPEG`

The source JPEG is 919×1536 RGB and matches the reference previously measured into `HomeRef.h`. Runtime target resolution is exactly 480×800 portrait.

## 2. Non-goals for v1

- Do not theme Reader, Settings, plugin pages, or other Fengyan/Lyra screens.
- Do not execute JavaScript/Lua/templates/arbitrary expressions on-device.
- Do not parse authoring JSON on normal device boot.
- Do not require a whole theme pack or a whole 480×800 raster in heap.
- Do not remove the current hard-coded Home path until the package path is verified.

## 3. Architectural rules

1. **Theme owns appearance and geometry.** Slot rectangles, focus rectangles, touch hit areas, static assets, typography style, radius and focus order belong to the theme.
2. **C++ owns data and behavior.** Recent-book data, Wi-Fi state, and callbacks remain application code.
3. **One geometry definition.** The same compiled slot/interactive record drives drawing, focus, and hit-testing. Do not keep parallel `HomeRef` draw and touch coordinates once migration is complete.
4. **Same runtime format for built-in and SD.** The built-in default is the exact `.m4theme` byte stream embedded in PROGMEM. An SD theme is read by seek/range from the same format.
5. **Fail closed.** Missing/corrupt/incompatible external themes fall back to the built-in theme without breaking Home.
6. **Streaming first.** Providers expose `readAt(offset, dst, len)`; background assets are read row-by-row or in small bounded chunks.
7. **No stale reference content.** Because the supplied JPEG contains sample covers/text/progress, the compiler must clear explicit dynamic regions before emitting the static background.

## 4. Authoring package

A source theme is a normal directory:

```text
themes/mofei-classic/
├── theme.json
├── assets/
│   └── home_reference.jpeg
└── preview/                # optional host-only material
```

`theme.json` is the source of truth. It is never required by the runtime.

### 4.1 JSON schema, v1

Illustrative shape:

```json
{
  "format": 1,
  "id": "mofei-classic",
  "screen": [480, 800],
  "background": {
    "source": "assets/home_reference.jpeg",
    "fit": "stretch_to_screen",
    "threshold": 170,
    "erase_regions": [
      [34,121,177,261],
      [226,155,220,205],
      [43,418,110,191],
      [185,418,110,191],
      [324,418,110,191],
      [425,14,45,42]
    ]
  },
  "slots": [
    {
      "id": "hero_cover",
      "type": "cover",
      "binding": "recent[0].cover",
      "rect": [37,124,171,254],
      "radius": 4,
      "focusable": true,
      "focus_order": 0,
      "target": {"type":"recent_book","index":0}
    },
    {
      "id": "hero_title",
      "type": "text",
      "binding": "recent[0].title",
      "rect": [230,160,214,52],
      "font": "ui_12_bold",
      "align": "left"
    },
    {
      "id": "hero_progress",
      "type": "progress",
      "binding": "recent[0].progress",
      "rect": [228,336,199,18],
      "radius": 9
    },
    {
      "id": "fanqie_action",
      "type": "hitbox",
      "rect": [247,650,86,92],
      "focusable": true,
      "focus_order": 6,
      "target": {"type":"action","action":"open_fanqie"}
    }
  ]
}
```

`erase_regions` are explicit instead of blindly erasing every slot. This prevents clearing nearby static borders/lines. The first theme should use the already-measured Home geometry and only widen erase rectangles enough to remove sample dynamic pixels.

## 5. Finite v1 vocabulary

### 5.1 Slot types

- `text`
- `image` / `icon`
- `cover`
- `progress`
- `hitbox`

No arbitrary runtime property bags are required. The host compiler normalizes authoring values into fixed typed records.

### 5.2 Bindings

Runtime enum `HomeThemeBinding` is finite. Initial values:

- `None`
- `WifiState`
- `RecentCountLabel`
- `Recent0Title`, `Recent0Author`, `Recent0Source`, `Recent0Cover`, `Recent0Progress`
- same for Recent1..Recent3 as applicable
- optional fixed app visual state bindings for File / WeRead / Fanqie / Jinjiang

The compiler maps friendly JSON strings such as `recent[0].title` onto enum IDs. Unknown bindings are compile errors.

### 5.3 Interactive targets

Runtime enum/struct is typed and independent from slot-array order:

```cpp
enum class HomeThemeTargetKind : uint8_t { None, RecentBook, Action };
enum class HomeThemeAction : uint8_t {
  OpenFiles, OpenWeread, OpenFanqie, OpenJinjiang,
  OpenHistory, OpenApps, OpenSettings
};
struct HomeThemeTarget {
  HomeThemeTargetKind kind;
  uint8_t index;          // RecentBook index when applicable
  HomeThemeAction action; // Action when applicable
};
```

Focusable records include `focus_order`. Keyboard/swipe navigation traverses sorted focus order. Touch hit-testing returns the same typed `HomeThemeTarget`. `HomeActivity` dispatches targets to existing callbacks. Theme authors may reorder buttons without changing C++ action semantics.

## 6. Runtime `.m4theme` format

One file, little-endian, no compression in v1. Magic is `M4TH`.

### 6.1 Header

A fixed header includes:

- magic `M4TH`
- format version = 1
- header size
- total file size
- screen width = 480
- screen height = 800
- section count
- flags
- payload CRC32
- reserved bytes for compatible extension

### 6.2 Section table

Each fixed-size descriptor contains:

- section type
- flags
- file offset
- byte length
- record count (when applicable)
- CRC32 or zero if covered by global payload CRC

Required/known section types in v1:

- `META`
- `STRINGS`
- `SLOTS`
- `ASSETS`
- `ASSET_DATA`

Unknown required sections reject the pack. Unknown optional sections may be skipped in future versions.

### 6.3 Slot record

A fixed-size packed record contains only runtime-normalized values:

- slot type
- binding enum
- rect x/y/w/h (uint16/int16 bounded to 480×800)
- font/style/alignment IDs
- radius/stroke/focus inset
- asset ID if used
- focusable + focus_order
- typed target kind/index/action
- optional string offset for fixed literal labels

### 6.4 Asset record

- asset ID
- asset type (`Raster1bpp`, `Icon1bpp`, optional black-only overlay)
- width/height/stride
- data offset/length
- flags

1bpp raster rows use exactly `(width + 7) / 8` bytes, MSB first, with a documented meaning for black bits. A full 480×800 background is 48,000 bytes.

### 6.5 Size bounds

V1 validator enforces conservative limits before any large read:

- total pack <= 256 KiB
- slots <= 64
- assets <= 32
- strings <= 8 KiB
- dimensions must be 480×800
- all offsets and lengths must be inside file bounds without overflow
- all rects must be valid and on-screen
- focus orders must be unique among focusable entries

## 7. Runtime providers

Create a small `IHomeThemeSource` interface:

```cpp
class IHomeThemeSource {
 public:
  virtual ~IHomeThemeSource() = default;
  virtual uint32_t size() const = 0;
  virtual bool readAt(uint32_t offset, void* dst, size_t len) const = 0;
};
```

Implementations:

- `BuiltinHomeThemeSource` — reads the generated PROGMEM byte array.
- `SdHomeThemeSource` — owns/uses `FsFile`, seeks and reads ranges via `SdMan`.

Do not allocate `total_size` bytes in heap.

External v1 active path:

`/.crosspoint/themes/active.m4theme`

If absent or validation fails, select the built-in provider.

## 8. Parser and validation

`HomeThemePackage` parses only bounded metadata and slot/asset tables into small fixed/bounded structures. Validation order:

1. source exists and minimum size
2. header magic/version/header size
3. total size and 480×800 resolution
4. section table bounds/non-overlap
5. global CRC32
6. record counts and record bounds
7. slot semantic checks, valid binding/target enum values, valid focus order
8. asset semantic checks

Any external failure records a diagnostic reason and falls back to built-in. A corrupt external file must never make Home blank or unresponsive.

## 9. Rendering pipeline

`HomeThemeRenderer` owns four explicit phases:

1. `drawBackground()`
2. `drawDynamicSlots(data)`
3. `drawOverlayAndMasks()`
4. `drawFocus(target)`

### 9.1 Background streaming

The 1bpp background is streamed using a 60-byte row buffer for 480 pixels. Clear the Home framebuffer region to white first. Decode each row into black runs and call `fillRect(runX, y, runW, 1, true)` (or a narrowly-scoped packed-row renderer helper if profiling proves necessary). This avoids a 48 KiB temporary background allocation.

### 9.2 Text

Text bindings use existing `M4UiText`/built-in font IDs. Compiler only permits known font aliases mapped to existing IDs/families. Text is clipped/truncated to the slot rect.

### 9.3 Covers and rounded clipping

Keep the current cover cache/BMP decoding path. Render flow for a cover slot:

1. render rectangular cover into `rect`
2. erase the four outside-corner pixels/spans to white using a precomputed radius inset table
3. redraw the rounded 1px border/chrome
4. draw focus later, never as part of the base cover

Corner clipping is procedural; no alpha bitmap and no full cover mask is stored. Typical radius tables are only a few bytes and can be generated by the compiler or runtime for supported radii.

### 9.4 Overlay

Static black chrome may be baked into background whenever possible. Optional overlay assets are black-only decorations. White erasure is always procedural (`fillRect(..., false)` / corner spans) because the existing 1bpp bitmap path does not restore white pixels.

## 10. Home data boundary

Introduce a small `HomeThemeViewModel` (or similarly named POD/view) that translates existing `RecentBook` + device state into the finite bindings. It does not own navigation callbacks.

`HomeActivity` responsibilities after migration:

- load/update recent data as today
- obtain active `HomeThemePackage`
- feed `HomeThemeRenderer` a `HomeThemeViewModel`
- hold current `HomeThemeTarget` focus
- ask renderer/package geometry for hit target / focus traversal
- map target to existing actions (`onAppsOpen`, `onSettingsOpen`, recent open, etc.)

The existing hard-coded Fengyan Home renderer remains callable as a legacy fallback until package renderer is verified.

## 11. First `mofei-classic` theme

The compiler takes the exact supplied JPEG, scales it deterministically to 480×800, thresholds it to 1bpp, then applies explicit erase regions for:

- hero cover
- hero title/author/source/progress text and progress fill
- three mini covers and mini titles
- recent count if dynamic
- Wi-Fi status region
- any other pixels whose sample value would become stale

Static card frames, section labels, separators, shortcut chrome/labels, and footer chrome may remain in the baked background if they are intentionally fixed in this theme. Dynamic cover borders are restored procedurally after cover draw.

The authoring JSON carries the measured slot rects from the current `HomeRef` so the new renderer initially reproduces the already-validated layout.

## 12. Build / tooling

Host tool:

`firmware/tools/compile_home_theme.py`

CLI:

```text
python3 tools/compile_home_theme.py \
  --theme ../themes/mofei-classic/theme.json \
  --out build/mofei-classic.m4theme \
  --emit-header src/generated/mofei_classic_m4theme.h
```

Responsibilities:

- strict JSON/schema validation
- image load/scale/threshold
- erase region application
- 1bpp packing
- JSON binding/action/font mapping to finite IDs
- pack layout/CRC generation
- optional C++ PROGMEM header generation from exact pack bytes
- deterministic output

Pillow is a host-build/test dependency only; device firmware has no Pillow/JSON dependency.

## 13. Testing requirements

Before production integration, tests must cover:

- compiler deterministic output and schema rejection
- JPEG scales to exactly 480×800 and dynamic erase regions are white
- binary pack header/section/CRC correctness
- C++ parser accepts known-good compiler fixture
- parser rejects bad magic/version/resolution/CRC/truncated/overflowed sections
- memory provider and SD/range provider equivalence
- target hit-test/focus-order semantics independent of slot array order
- rounded cover corner erase spans
- built-in fallback when external theme is absent/corrupt
- existing Home focused tests remain green or are intentionally migrated
- full `pio run -e murphy_m4`
- QEMU/plugin build and real simulator framebuffer screenshot

## 14. Migration / rollback

Phase 1 adds compiler, format, package providers and renderer without deleting legacy Home.
Phase 2 adds the first built-in package and tests it behind `HomeActivity` package availability.
Phase 3 routes Fengyan Home through the package renderer by default, with automatic legacy/built-in fallback on package failure.
Phase 4, only after simulator and hardware confidence, may delete redundant HomeRef/Fengyan-specific drawing code.

Rollback is always possible by forcing the existing legacy Home renderer; external SD themes never replace or mutate the embedded built-in bytes.
