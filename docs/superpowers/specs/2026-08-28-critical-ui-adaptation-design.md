# Murphy M4 Critical UI Adaptation

## Status and intent

This document defines a critical adaptation of the external contributor's UI
ideas into the real Murphy M4 firmware. It is a design boundary, not an
implementation plan or a commitment to reproduce the contributor preview
pixel-for-pixel.

The current canonical baseline is `76e72ec316321c8507df0bbb3eb11babdbfdbebf`,
on top of `faca95d6618791d5e10295a608c484b2518ba5ea`. The implementation will
be developed on `feature/critical-ui-adaptation`.

## Goal

Give the real 480x800 Murphy M4 UI a clearer, more deliberate monochrome
visual grammar inspired by the contributor preview, while preserving the
existing theme architecture, touch behavior, navigation, provider contracts,
and reader reliability.

The result must improve functional clarity on an e-ink device: users should be
able to identify the current selection, primary action, metadata, and page
structure quickly without depending on color, dense decoration, or browser
layout behavior.

## Non-goals

- Do not copy the external branch wholesale or cherry-pick `a0bb304`.
- Do not create a new UI framework, browser runtime, or CSS/layout engine.
- Do not initially add a new global theme; adapt the existing theme seams.
- Do not redesign reader pagination, reader typography, or page-turn behavior.
- Do not change Wi-Fi, plugin business logic, WeRead, lifecycle, partitions,
  flashing, or runtime font loading.
- Do not bundle fonts, uncertain third-party icons, screenshots, or generated
  preview artifacts.
- Do not optimize for exact visual equivalence with 400x800 web previews.

## External contribution context and provenance

External commit `a0bb3041c97e41263cf5afc30a83352c5135d78e` is a preview-only
contribution. Its parent is `78a99d082bb35a845d92a01c3def7cd86128739e`; the
commit adds a large `m4ui_all.py` renderer, a 58-screen HTML generator,
preview images, generated icon caches, and SimpleUI-derived SVGs. It does not
constitute production firmware UI code.

The external preview uses a 400x800 HTML contract in places, while M4's
production panel is 480x800. Its SimpleUI-derived assets do not establish an
independent license/provenance chain suitable for firmware distribution. The
adaptation therefore uses the external work as visual research only and
reuses current repository geometry, drawing primitives, fonts, and icons.

## Critical adaptation boundaries

| External idea | Decision | Critical adaptation and reason |
| --- | --- | --- |
| Strong information hierarchy on home/detail screens | KEEP | Aligns with existing HomeActivity and NativeProviderBookActivity flows and improves task recognition without changing actions. |
| Text-first provider/detail layout | KEEP | Fits Chinese readability and e-ink density; avoids requiring covers or large image memory. |
| Monochrome black/white/light-gray visual language | KEEP | Works with the panel and existing dither/ink primitives. |
| Selected rows/cards with clear contrast | KEEP | Adapt to existing selected-state drawing and touch geometry; use limited filled regions to control ghosting. |
| Sparse section rules and metadata grouping | KEEP | Improves scanning if implemented with existing renderer lines and current metrics. |
| Category/provider tile grouping | ADAPT | Retain the grouping concept, but derive cell geometry from current 480x800 metrics and existing touch layouts rather than copying HTML dimensions. |
| Book cards with covers and progress | ADAPT | Use existing lazy cover rendering, recent-book buffers, and bounded metadata. Covers remain optional and must not become a navigation prerequisite. |
| Pixel-dot background | ADAPT | At most use sparse, low-ink decoration in a bounded region; default screens should avoid full-page dot fields because they add redraw/ghosting noise and reduce text contrast. |
| Pixel logo/header treatment | ADAPT | Preserve `GUI.drawHeader`, battery placement, title width, and header back hitbox. A small programmatic mark is acceptable only if it does not consume title or touch space. |
| Rounded cards and pill controls | ADAPT | Keep restrained radii for selection and primary actions using renderer primitives; avoid many filled or outlined surfaces on every row. |
| Spaced CJK typography and tracking | ADAPT | Apply only to short labels where width is proven. Continue using `M4UiText` CJK-safe width, wrapping, and truncation for all dynamic text. |
| Bottom navigation chips | ADAPT | Map the visual grouping onto `M4TouchNavigation`, footer masks, and existing button semantics; never introduce a competing navigation model. |
| HTML/CSS utility classes and browser layout | REJECT | Not a firmware runtime architecture; it adds no value on the device and conflicts with fixed-memory drawing. |
| 400x800 layout constants | REJECT | Production M4 is 480x800; copied constants would create clipping, excess whitespace, or hit-target mismatch. |
| `m4ui_all.py` monolith and generated screen suite | REJECT | Too large and disconnected from production ownership; it would duplicate layout logic and increase maintenance risk. |
| SimpleUI-derived SVG icon set and generated C++/Python caches | REJECT | Provenance/license is not independently established; generated assets also add flash/build complexity. Prefer existing icons or bounded primitives. |
| Tracked preview screenshots as firmware assets | REJECT | Screenshots are design references, not device resources, and their source/provenance is uncertain. |
| Bundled or Windows-specific fonts | REJECT | Increases storage and licensing risk and can fail on Chinese glyph coverage. Use the existing runtime UI font policy and repository/system-independent firmware fonts. |

## M4 constraints

### E-ink refresh and ghosting

Navigation screens must remain compatible with current fast/partial refresh
behavior. The design should minimize large filled areas, repeated decorative
patterns, and unnecessary redraws. Full refresh policy remains owned by the
existing display/lifecycle code; the visual adaptation must not add animation
or redraw loops merely to imitate a browser preview.

### Geometry and density

All production layout calculations target 480x800 portrait and derive from
`UITheme::getMetrics()` or shared geometry helpers. No external preview
constant may be copied without checking its relationship to current header,
footer, content, and row metrics. Content must remain dense enough for useful
lists while reserving real touch space.

### Chinese readability and fonts

Dynamic strings remain UTF-8-safe and use `M4UiText` for measuring, wrapping,
truncation, and fixed UI font roles. Decorative letter spacing must never
replace readable glyph sizing or reduce the available width for Chinese titles
and metadata below the existing contract.

### Touch and navigation

Visual rectangles and hit regions must continue to share existing geometry.
Touch targets should remain at least the current M4-sized targets, including
the header back area, Home menu grid, provider detail read action, list rows,
and bottom footer. Physical-button selection and touch selection must continue
to reach the same actions.

### Performance, RAM, and flash

Rendering remains streaming/virtualized: provider lists stay controller-backed
and covers stay lazy. Do not allocate full galleries, full provider datasets,
or per-screen image caches in firmware. New code should use existing
bitmaps/primitives and have a measured flash/RAM impact in the production
build. No preview tooling or generated assets belong in the firmware image.

### Theme consistency and maintainability

The adaptation should be expressed through existing `UITheme`,
`FengyanTheme`, `BaseTheme`, `M4UiText`, `M4UiStyle`, and `M4NativeUi` seams.
Avoid screen-local copies of metrics or a second style system. Lyra must
remain buildable and behaviorally intact even if the first visual pass is
Fengyan-oriented.

### Asset and licensing provenance

Only existing repository assets with established ownership, newly written
programmatic primitives, or assets with independently verified redistribution
rights may enter firmware. Do not copy external fonts, SVGs, generated icon
headers, or screenshots merely because they appear in the preview repository.

## Recommended architecture

Adapt the current component stack rather than introducing another runtime:

- `UITheme` and `BaseTheme` remain the dispatch and component contracts.
- `FengyanTheme` is the first visual target because it already owns the M4
  three-column menu, 32px icon family, selected-state options, header, list,
  tab, and cover drawing.
- `M4UiText` remains the sole path for UI text measurement and CJK-safe
  truncation/wrapping.
- `M4UiStyle` and existing geometry helpers provide normalized 480x800 metrics
  and hitbox calculations where a shared pure policy is useful.
- `M4NativeUi` and `NativeAppActivity` continue to render native provider
  screens through bounded nodes, virtualized lists, existing footer layout,
  and named actions.

The first implementation should add only small reusable visual primitives or
theme adjustments needed by the selected screens. It should not create a new
theme, parser, CSS abstraction, or icon framework.

## First implementation scope

1. Home recent-book/card treatment: improve hierarchy among cover, title,
   author/progress metadata, and selection while preserving lazy cover buffers,
   current menu ordering, and Home touch mapping.
2. Native provider home/category/list: apply restrained section/category
   grouping and selected-row contrast through existing native UI and theme
   paths. Keep provider data, paging, and actions unchanged.
3. `NativeProviderBookActivity` detail hierarchy: preserve the existing title,
   author/meta, resume state, primary read action, recent updates, intro, and
   footer behavior while making the hierarchy visually clearer.

Reader screens, settings, Wi-Fi, plugin runtime surfaces, and WeRead are
regression surfaces for this work, not initial redesign targets.

## Explicitly preserved behavior

- `M4TouchNavigation`, `TouchHitGeometry`, footer masks, and provider detail
  touch policies remain authoritative for hit testing.
- Reader entry, pagination, line breaking, page-turn animation, and reader
  settings remain unchanged.
- Wi-Fi state, credentials, network flows, and lifecycle behavior remain
  unchanged.
- Native and Lua provider business logic, provider actions, catalog/chapter
  loading, and plugin compatibility remain unchanged.
- WeRead routing and reader handoff remain unchanged.
- The existing runtime font engine and fixed UI font policy remain unchanged.
- Partition definitions, flashing workflows, boot slots, and device update
  behavior remain unchanged.

## Styling grammar

The first pass uses a restrained monochrome grammar:

- white background and black primary ink;
- light gray/dither only for secondary metadata or selected-state support;
- sparse one-pixel dividers between semantic sections;
- clear selected-state contrast, with filled regions limited to the active row
  or primary action;
- modest, consistent corner radii only where they improve grouping;
- readable Chinese titles and metadata using current font roles;
- no full-page dot grid, no browser-only decoration, and no mechanical copy of
  external spacing, dimensions, iconography, or assets.

## Verification gates

Each behavioral change should follow TDD where practical: add a focused
contract test, observe a real failing result for the new behavior, implement
the smallest change, then run the green test and relevant regressions.

The branch must pass, as applicable:

- focused host/native tests for geometry, visual-state policies, CJK text
  bounds, and representative Home/provider detail contracts;
- existing touch, native UI, provider, reader lifecycle, Wi-Fi, WeRead, and
  font regression tests;
- `python3 -m py_compile` for changed host tools, if any host tooling is
  touched;
- a fresh production `murphy_m4` build with RAM/flash usage reviewed;
- the mandatory established simulator smoke for every major change:
  `./m4sim test smoke --plugin-debug --skip-build --ready-seconds 90`;
- screenshot/frame comparison of key Home, provider-list, and provider-detail
  states at actual 480x800 dimensions when the simulator path supports it;
- real-device navigation/touch/screenshot checks only after host tests,
  production build, and simulator smoke are green;
- `git diff --check` and a scope check confirming no reader, plugin, network,
  partition, or flashing changes slipped into the adaptation.

Visual review must distinguish actual captured frames from host approximations;
no visual-equivalence claim is valid without a successful frame capture.

## Phase and commit decomposition

Use one focused branch with three or four reviewable implementation commits:

1. Shared visual grammar/policy primitives and focused contract tests.
2. Home recent-book/menu adaptation and Home geometry regression coverage.
3. Native provider home/list/detail adaptation and provider touch/UI coverage.
4. Verification/documentation updates, only if needed after the feature commits.

The design document is independent of those implementation commits. No commit
should mix this work with font compatibility, runtime diagnostics, provider
logic, Wi-Fi, lifecycle, or unrelated cleanup.

## Rollback strategy

Each phase must be revertible by commit. Existing theme/component behavior is
the fallback: if a visual primitive causes clipping, ghosting, memory growth,
or touch regressions, remove the phase or disable the affected component while
retaining the prior drawing path. Do not roll back by changing reader,
provider, or device-update behavior. A failed simulator or hardware gate
blocks the next phase until the affected visual change is corrected or
reverted.

## Acceptance criteria

The adaptation is acceptable only when all of the following are true:

- Home, native provider list/category, and provider detail have a coherent
  monochrome visual hierarchy materially clearer than the baseline.
- All key screens render inside the 480x800 contract with readable Chinese
  titles, metadata, and actions.
- Selection, back, footer, list, category, and primary read touch targets map
  to the same actions as before and retain usable hit sizes.
- No full-page dot-grid dependency, uncertain asset/font, browser runtime, or
  copied 400x800 constant is introduced.
- Production RAM/flash budgets remain acceptable and no runtime/plugin/network/
  reader behavior changes are present.
- Focused tests and relevant regressions are green, the production build is
  successful, and the mandatory simulator smoke passes.
- Real-device checks, performed only after simulator success, show stable
  navigation and reader handoff; any visual claim is backed by a captured
  480x800 frame.
- The final diff is limited to the approved UI/theme/test/documentation scope
  and can be reverted phase by phase.
