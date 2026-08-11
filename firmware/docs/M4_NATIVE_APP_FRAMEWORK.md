# M4 Native App Framework v1

## Goal

Make native C++ the default runtime for content-heavy M4 apps. Lua remains an optional runtime for small games, calculators, demos and other light scripted tools.

The package still uses `.m4x`, but the package chooses its runtime:

```json
{
  "id": "com.fanqie.client",
  "name": "番茄小说",
  "version": "1.0.0",
  "versionCode": 100,
  "runtime": "native",
  "entry": "main.xml",
  "provider": "fanqie",
  "permissions": ["network", "filesystem.appdata"]
}
```

Legacy packages omit `runtime` and continue to default to Lua + `main.lua`.

## Separation of responsibilities

### XML / Native UI

XML describes layout only. It cannot:

- execute expressions or loops;
- issue HTTP requests;
- access cookies/tokens;
- read arbitrary files;
- allocate provider-sized data structures;
- call Lua.

The renderer asks a native controller for scalar values and virtualized list rows, then dispatches named actions back to that controller.

### Native Provider

A provider owns:

- authentication/credential references;
- shelf/category/search data;
- book metadata and persistent catalog;
- chapter HTTP and redirects;
- JSON streaming extraction;
- WeRead shard decode / XHTML strip;
- JJWXC GB18030 conversion / VIP flow;
- chapter cache `.part` / `.ok` state;
- prefetch and retry scheduling;
- progress persistence and history reopen.

A normal reader path must not require Lua to be resident.

### Lua

Lua is optional and intended for bounded lightweight apps such as:

- calculator;
- small board/card games;
- clocks and simple utilities;
- experimental scripted pages.

A native provider may explicitly request a login UI route, but heavy chapter networking/decoding must not fall back to Lua.

## M4 UI XML v1

The v1 grammar is intentionally smaller than general XML. Components are self-closing, text content is carried in attributes, and no DTD/CDATA/namespaces are accepted.

```xml
<?xml version="1.0"?>
<m4ui version="1" start="shelf">
  <screen id="shelf" title="@app.name">
    <tabs
      source="provider.categories"
      onChange="provider.selectCategory" />

    <text text="@page.status" bold="true" height="36" />

    <list
      id="books"
      source="provider.books"
      titleField="title"
      subtitleField="author"
      valueField="progress"
      onActivate="provider.openBook"
      pageSize="8" />

    <buttons
      back="返回"
      primary="打开"
      left="上一页"
      right="下一页"
      onBack="system.back"
      onPrimary="provider.openSelected"
      onLeft="provider.prevPage"
      onRight="provider.nextPage" />
  </screen>

  <screen id="loading" title="@book.title">
    <text text="@loading.phase" bold="true" />
    <progress source="@loading.percent" max="100" />
    <text text="@loading.detail" />
    <buttons back="取消" onBack="provider.cancel" />
  </screen>
</m4ui>
```

### Supported components

- `text`: one system-styled text row.
- `flowText`: system body text surface. The XML contract is stable while the implementation can move to the same line-breaking engine used by EPUB/reader text.
- `list`: virtualized native list; rows come from a controller data source.
- `tabs`: native theme tab bar.
- `progress`: native progress bar.
- `spacer`: fixed vertical spacing.
- `divider`: system divider.
- `buttons`: system bottom button/touch-navigation labels and actions.

### Bindings

A string beginning with `@` is a scalar binding, for example:

- `@app.name`
- `@book.title`
- `@loading.phase`

A list or tab source names a virtual native data source:

- `provider.shelf`
- `provider.categories`
- `provider.searchResults`
- `provider.toc`

The XML never materializes the full list. `rowCount()` and `rowAt()` are native controller calls; large TOCs can stay in `FileRows` on SD.

### Actions

Actions are symbolic names, not code:

- `system.back`
- `provider.openBook`
- `provider.openChapter`
- `provider.selectCategory`
- `provider.nextPage`
- `provider.retry`
- `provider.login`

Each controller implements an explicit allow-list. Unknown actions fail closed.

## UI reuse

Native XML pages use the existing system UI stack:

- `UITheme` / current Fengyan or Lyra theme;
- `M4UiText` so UI reuses the selected reader TTF face rather than loading another runtime TTF;
- `GUI.drawHeader`, `drawList`, `drawTabBar`, `drawProgressBar`, `drawButtonHints`;
- shared touch geometry and system navigation behavior.

For long text blocks, `flowText` is the abstraction boundary for reusing the EPUB/TXT reader typography and line-break implementation. Package XML does not own pagination rules.

## Memory rules

- XML entry: <= 32 KiB.
- <= 12 screens; <= 48 component nodes per screen.
- <= 256 bytes per attribute string.
- package UI XML is parsed once, preferably from PSRAM-backed input storage.
- list data is virtualized and must not be copied into the UI document.
- rendering callbacks must be cache/SD reads only; no blocking network in `render()`.
- TLS, content decode and reader bootstrap are scheduled outside the UI renderer.

## Reader handoff

The target path is:

```text
Native XML shelf/detail
  -> NativeProviderManager.register/openBook
  -> persistent BookSpec/catalog/auth-profile reference
  -> native TOC / loading
  -> TxtReaderActivity
```

History opens `m4cp://<provider>/<book>` directly through `NativeProviderManager`; no package runtime is required.

## Cache correctness

Progressive content uses a growing-file contract:

```text
chapter.txt.part
  + availableBytes
  + complete=false
```

The reader may index only the currently available range and extend its index as the file grows. Completion atomically promotes to the formal cache and writes `.ok`. A positive partial file size must never be treated as a complete chapter.

## Migration

1. Keep existing Lua packages working unchanged (`runtime` defaults to `lua`).
2. Introduce native XML runtime and controller/provider interfaces.
3. Port Fanqie first (simple JSON streaming path).
4. Port JJWXC (free JSON + native WAP GB18030 path).
5. Port WeRead using the existing native protocol helpers originally ported from `papers3-weread`, replacing whole-string shard decode with file/window streaming.
6. Once a provider is fully native, remove its Lua network/content path; retain Lua only where a login or optional scripted utility genuinely needs it.
