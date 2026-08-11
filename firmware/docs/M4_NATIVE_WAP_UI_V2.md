# M4 Native WAP UI v2

## 1. Purpose

M4 Native UI should feel as easy to author as a small WAP page, without turning an ESP32-S3 reader into a browser engine.

The design targets four properties at the same time:

1. **HTML-like authoring** — packages describe pages, bindings, navigation and semantic style in XML.
2. **Native execution** — networking, authentication, decode, cache, catalog, search and reader handoff stay in C++.
3. **Deterministic memory** — no DOM mutation, JavaScript VM, CSS cascade, whole-response JSON DOM or unbounded list model.
4. **Reader-quality typography** — UI and long text reuse the system font/runtime TTF and converge on the same CJK flow rules used by EPUB/TXT.

This is intentionally closer to **WAP + Android XML + ViewModel** than to a general browser.

---

## 2. Runtime model

```text
.m4x package
  ├── manifest.json
  ├── main.xml
  └── small static assets / protocol tables
          │
          ▼
Bounded XML parser
  ├── <= 32 KiB
  ├── <= 12 screens
  ├── <= 48 nodes/screen
  └── class tokens -> StyleFlags
          │
          ▼
NativeAppActivity
  ├── UITheme / GUI
  ├── M4UiText / runtime TTF
  ├── virtual lists
  └── symbolic actions
          │
          ▼
Native Controller
  ├── scalar bindings
  ├── FileRows data sources
  ├── navigation
  └── allow-listed provider actions
          │
          ▼
Native Provider Services
  ├── Discovery / Search
  ├── Catalog / TOC
  ├── Login / CredentialStore
  ├── Chapter streaming
  ├── Decode / XHTML / GB18030
  ├── Cache / Progress
  └── Reader handoff
```

The XML layer never owns sockets, credentials or provider protocol logic.

---

## 3. M4UI is not HTML and does not execute script

Allowed page markup stays intentionally small:

```xml
<m4ui version="1" start="home" theme="wap">
  <screen id="home" title="晋江文学城" class="compact">
    <text text="我的书架" class="hero" />
    <text text="@page.status" class="meta inset" />
    <divider class="hairline inset" />
    <list
      source="provider.books"
      titleField="title"
      subtitleField="author"
      valueField="progress"
      onActivate="provider.openBook"
      class="inset" />
    <buttons
      back="返回"
      primary="阅读"
      left="频道"
      right="排行"
      onBack="system.back"
      onPrimary="provider.openSelected"
      onLeft="ui.go:channels"
      onRight="ui.go:rank" />
  </screen>
</m4ui>
```

An action such as `ui.go:rank` or `provider.openBook` is a symbolic identifier. It is not an expression and cannot invoke arbitrary functions.

Forbidden:

- `<script>`;
- expressions such as `a + b`;
- loops or package-defined functions;
- arbitrary file access;
- arbitrary HTTP URLs from XML;
- direct credential access;
- dynamic native code;
- calling Lua from a native page.

Unknown actions fail closed.

---

## 4. CSS-like styling without a CSS engine

A real CSS parser is the wrong trade-off for ESP32-S3. M4UI therefore uses **utility classes compiled to bit flags at parse time**.

Current semantic tokens:

| Token | Meaning |
| --- | --- |
| `compact` | reduced vertical rhythm |
| `hero` | primary page heading |
| `section` | small bold section heading + rule |
| `meta` / `muted` | secondary text role |
| `center` | centered text |
| `inset` | extra side padding |
| `ranked` | numeric ranking prefix for list rows |
| `hairline` | compact divider |

Example:

```xml
<text text="本周金榜" class="section inset" />
<list source="provider.rank" class="ranked inset compact" />
```

At runtime this is only a `uint16_t StyleFlags`. There are no selectors, specificity, cascading, inheritance, style tree or layout recalculation.

### Why semantic classes

E-ink UI benefits more from stable hierarchy than from arbitrary visual decoration. Packages should express intent (`hero`, `section`, `meta`) and let the firmware/theme choose exact metrics.

This also lets Fengyan/Lyra/new themes change rendering without rewriting packages.

---

## 5. Page information architecture

### 5.1 JJWXC

JJWXC's mobile/WAP information architecture is naturally suitable for e-ink because it is text dense.

Target pages:

```text
Home / Shelf
  ├── Continue/open local books
  ├── Channels
  │    ├── 古言
  │    ├── 现言
  │    ├── 纯爱
  │    ├── 衍生
  │    ├── 无CP
  │    ├── 百合
  │    └── 完结
  ├── Find Books
  │    ├── 分类
  │    ├── 全本
  │    ├── 免费
  │    └── Search
  └── Ranking
       ├── 频道金榜
       ├── VIP 新文
       ├── VIP 完结
       ├── 勤奋更新
       ├── 月度榜
       └── 季度榜
```

Do **not** recreate desktop cover grids. A JJ row should normally be:

```text
作品名                         状态/进度
作者 · 标签/最近更新
```

VIP status belongs to metadata, not a second page layout.

### 5.2 Fanqie

Target pages:

```text
Home / Shelf
  ├── Discover
  │    ├── 推荐
  │    ├── 男频 / 女频
  │    ├── 分类
  │    ├── 新书
  │    └── 完结
  ├── Ranking
  │    ├── 阅读榜
  │    └── 新书榜
  └── Search (later)
```

The web product uses large cover cards, but M4 should preserve the useful metadata and remove the expensive presentation:

```text
01  作品名                       在读/状态
    作者 · 分类 · 最近更新
```

Covers should be optional and lazy. They must never be required for navigation or discovery.

### 5.3 WeRead

WeRead should remain the simplest:

```text
Home
  ├── Continue Reading
  ├── Shelf
  ├── Search
  └── Login / account state
```

The main screen should optimize for one action: continue reading.

---

## 6. Data source contract

UI lists are virtual. The controller exposes:

```cpp
size_t rowCount(const std::string& source) const;
bool rowAt(const std::string& source, size_t index0, Row& out) const;
```

Large online responses are streamed directly to transactional FileRows:

```text
HTTPS body
  -> incremental JSON extractor
  -> shelf_rows.tsv.tmp / rank_rows.tsv.tmp
  -> flush + validate
  -> atomic rename
  -> UI rowCount/rowAt
```

The UI never holds all books in a `std::vector<Book>`.

Recommended standard row schema:

```text
id<TAB>title<TAB>subtitle<TAB>value<TAB>flags
```

`flags` may later encode provider-neutral states such as VIP, finished, cached or new.

### Data source namespaces

```text
provider.shelf
provider.recommend
provider.categories
provider.rank
provider.searchResults
provider.toc
```

Provider-specific protocol fields must be normalized before reaching the UI.

---

## 7. Discovery and fresh-install contract

A zero-Lua native package must be useful on a clean SD card.

Required flow:

```text
Native app enter
  -> local FileRows available?
       yes -> paint immediately
       no  -> startDefault(provider, appId)
  -> paint loading/skeleton state
  -> streaming discovery writes .tmp
  -> atomic publish
  -> controller refreshes row count
```

Defaults:

- WeRead: authenticated cloud shelf; if auth is absent, show login state.
- Fanqie: one public default recommendation/category channel.
- JJWXC: one public default channel; login is not required for public discovery/free chapters.

Opening a discovery result must bootstrap its catalog natively:

```text
bookId
  -> native TOC request
  -> toc_rows.txt.tmp
  -> validate row count
  -> .ok / atomic publish
  -> persistent BookSpec
  -> NativeProviderBookActivity
```

`book_catalog_missing` is not an acceptable normal fresh-install UX.

---

## 8. Shared FlowLayout instead of a second typography engine

`flowText` is currently a small bounded UI renderer. The target is to extract a shared typography primitive from EPUB/TXT rather than grow a parallel UI line breaker.

Proposed API:

```cpp
struct FlowStyle {
  int fontId;
  int maxWidth;
  int lineHeight;
  int firstLineIndent;
  int paragraphSpacing;
  bool cjkPunctuationRules;
};

class M4FlowLayout {
 public:
  FlowMeasure measure(TextSource&, const FlowStyle&, size_t start, int maxLines);
  FlowPaintResult paint(GfxRenderer&, TextSource&, const FlowStyle&,
                        size_t start, const Rect& clip);
};
```

`TextSource` can be either:

- a bounded UI string;
- a range reader over a file;
- an EPUB parsed text block.

Shared behavior should include:

- UTF-8 character boundaries;
- CJK punctuation line-start/line-end rules;
- western word wrapping;
- runtime TTF metrics;
- paragraph spacing/indent;
- no whole-document materialization.

The reader remains the source of truth for body typography. M4UI consumes that engine; it does not fork it.

---

## 9. HeavyResourceCoordinator

A plain mutex is the first safety layer. The target scheduler is explicit and priority aware.

```cpp
enum class HeavyResource {
  None,
  Tls,
  Decode,
  ReaderBootstrap,
  Script,
};

enum class WorkPriority {
  BackgroundPrefetch,
  UserVisible,
  ForegroundBlocking,
};
```

Rules:

1. Only one heavy resource owner exists process-wide.
2. Foreground work can ask background prefetch to cancel at its next bounded checkpoint.
3. TLS and large content decode do not overlap.
4. Reader bootstrap pauses background prefetch.
5. Login holds the resource only for each HTTP transaction, **not** while waiting for QR scan.
6. Discovery uses the same coordinator as chapter fetch.
7. Every loop doing network/decode must have cancellation checkpoints.

The existing `M4NativeProviderHeavyGate` is the mutex-level implementation of rule 1; v2 should evolve it into this coordinator rather than create multiple independent locks.

---

## 10. Lua policy: ephemeral escape hatch only

Native reading packages should require no Lua at all.

If a future app genuinely needs scripted logic, Lua follows an **ephemeral VM** contract:

```text
request Script resource
  -> verify no TLS/decode/reader-bootstrap owner
  -> create bounded Lua VM
  -> execute one small task
  -> serialize only small result/state
  -> lua_close()
  -> release Script resource
```

A script task must not:

- own a TLS client;
- hold a provider response body;
- keep chapter text in Lua strings;
- remain alive while native TLS starts;
- become the reader owner.

For a native package, the network Lua bindings should be unavailable by construction. If a native Provider needs an unusual protocol step, implement a native adapter/helper first; Lua is the last resort, not the normal extension API.

Acceptance target:

```text
M4_ENABLE_LUA=0
```

must still support JJWXC, Fanqie and WeRead end-to-end.

---

## 11. Page lifecycle / memory hibernation

A shelf page is not worth keeping alive while reading.

Target transition:

```text
NativeAppActivity
  -> persist tiny UI state (screen id, selected row, tab/filter)
  -> destroy XML Document
  -> destroy Controller and transient FileRow indexes
  -> NativeProviderBookActivity
  -> Reader
```

Return:

```text
Reader close
  -> reconstruct NativeAppActivity
  -> parse <=32 KiB XML again
  -> restore tiny state
```

Re-parsing a small XML file is cheaper and safer than retaining a UI tree during a long reader session.

The same rule applies to search result indexes: close/release them before reader bootstrap.

---

## 12. Memory budget

Recommended transient windows:

| Stage | Budget / strategy |
| --- | --- |
| XML input | <= 32 KiB, PSRAM first |
| UI document | bounded node/string counts |
| HTTP read | 4–8 KiB |
| JSON token/record | bounded extractor, <= 24 KiB internal |
| GB18030 conversion | ~4 KiB window + 47,760-byte table (PSRAM first) |
| file write | 4–16 KiB |
| list row | one row, <= ~2 KiB |
| runtime TTF glyph cache | PSRAM first, separately bounded |
| Lua escape hatch | fixed allocator quota; never overlaps heavy native stage |

Forbidden patterns:

```cpp
std::string body = http.getString();
JsonDocument doc(body);
std::string decoded = ...;
std::vector<Book> allBooks = ...;
```

when the response can be streamed.

---

## 13. Cache transaction rules

Every durable network result follows:

```text
name.tmp / chapter.txt.part
  -> bounded streaming write
  -> flush
  -> structural validation
  -> atomic rename / commit
  -> optional .ok marker
```

A partial chapter is never a cache hit merely because its size is non-zero.

The reader may support growing files later, but that requires an explicit `availableBytes + complete=false` contract and incremental index extension. It must never infer completion from current file length.

---

## 14. Logging policy

Release/user builds must not perform verbose diagnostic SD writes.

Font tracing is opt-in at compile time:

```text
-DM4_FONT_DIAGNOSTIC=1
```

Only developer builds write `/.crosspoint/logs/font_debug.log`, with a hard size cap/rotation. Normal builds keep concise Serial errors but do not log every TTF seek/read to SD.

The same policy should be used for provider protocol traces: event counters and bounded error snapshots are preferable to unbounded files.

---

## 15. Implementation phases

### Phase A — page protocol and visual hierarchy

- [x] `theme="wap"`
- [x] CSS-like utility classes -> bit flags
- [x] safe `ui.go:<screen>` navigation
- [x] numeric XML entities for compact multiline text
- [x] rewrite JJWXC/Fanqie/WeRead native home shells
- [x] release font diagnostics disabled
- [x] ship JJWXC `gbk_table.bin`
- [x] serialize chapter/Login TLS via heavy gate
- [x] fix JJWXC WAP entity state

### Phase B — make every page live

- [ ] implement `M4NativeProviderDiscovery.cpp`
- [ ] stream default shelf/recommend data into FileRows
- [ ] add category/rank/search query model
- [ ] native TOC bootstrap on first open
- [ ] controller loading/error/empty states without blocking render

### Phase C — typography and lifecycle

- [ ] extract `M4FlowLayout` from reader typography primitives
- [ ] make `flowText` use shared CJK line rules
- [ ] hibernate/destroy NativeAppActivity when entering book/reader
- [ ] reopen XML and restore tiny UI state on return

### Phase D — remove compatibility debt

- [ ] replace `TxtReaderActivity::PluginSession` with provider-neutral `ReaderBookSession`
- [ ] replace native use of `M4PluginReaderSession/Bridge`
- [ ] forbid AppRuntime/Lua imports from native provider/reader directories
- [ ] add Lua VM creation audit/tripwire
- [ ] add native-only firmware environment (`M4_ENABLE_LUA=0`)

---

## 16. Release acceptance criteria

JJWXC, Fanqie and WeRead native packages are release candidates only when all of the following are true:

1. Fresh install shows usable online data or a meaningful login state.
2. Shelf/discovery -> TOC -> chapter -> reader works with no Lua VM creation.
3. History reopen stays native.
4. Next/previous chapter stays native.
5. Login, chapter TLS and decode never overlap heavy-resource ownership.
6. Package audit finds no `.lua` in native package files.
7. `M4_ENABLE_LUA=0` build can use all three reading providers.
8. No provider response requires whole-body materialization for normal large paths.
9. UI document is released while reader owns the foreground.
10. Runtime TTF and UI use one font/metrics policy.
11. Release build performs no verbose SD diagnostics.
12. Hardware tests include slow SD, missing cache, expired auth, large TOC, large chapter and network retry scenarios.

The goal is not to emulate Chrome. The goal is a **small, deterministic native WAP runtime optimized for an e-ink reader**.
