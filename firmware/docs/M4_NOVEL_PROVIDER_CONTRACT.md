# M4 Novel Provider Contract

## Goal

M4 novel plugins should share one reader-quality host instead of reimplementing a small browser for every site.

The common lifecycle is:

```text
Explore / Search
      ↓
Book Detail
      ↓
Catalog
      ↓
Chapter
      ↓
Native Reader
```

JJWXC and Fanqie are the first two public providers using this shape. WeRead keeps its shelf-oriented home page but can share the same detail/catalog/reader contracts.

The design intentionally resembles the useful data model of Legado book sources while staying bounded enough for ESP32-S3.

## What the host owns

The firmware owns all reusable behavior:

- Native UI layout and runtime TTF rendering;
- category tiles, recommendation lists and standard book-detail page;
- touch/key navigation;
- one HTTP/TLS transport and heavy-resource coordination;
- transactional FileRows cache;
- loading/error/auth presentation;
- catalog picker;
- chapter cache;
- history/progress resume;
- TXT reader handoff and next-chapter prefetch.

A provider must not create its own Activity just to change a URL or JSON field.

## Provider stages

### 1. Explore

Input:

```text
category key + page/offset
```

Output is normalized `BookCard` rows:

```text
bookId<TAB>title<TAB>author<TAB>value<TAB>flags...
```

UI datasource names are stable:

```text
provider.categories
provider.recommend
provider.searchResults
```

The category key is opaque to the UI. Examples:

```text
JJWXC: 14000019
Fanqie: 1:516      # gender:category_id
```

This is the M4 equivalent of Legado `exploreUrl + ruleExplore`.

### 2. Book Detail

Normalized model: `M4NovelProvider::BookDetail`.

Useful fields:

```text
title
author
coverUrl        # optional URL; host downloads/decodes/caches it
intro
kind
status
wordCount
lastChapter
```

Only `bookId + title` are mandatory. The shared detail page remains usable when a source cannot cheaply provide all metadata.

`M4NovelProvider::BookCard::coverUrl` and `M4NovelProvider::BookDetail::coverUrl`
are the exact optional cover interface. Providers return the URL only; the host
owns bounded download, BMP conversion, cache naming, and `RecentBook.coverBmpPath`.

The page provides two host actions:

- **Start / Continue Reading** — restores the last provider chapter and byte offset when available, otherwise chapter 1;
- **Chapters** — opens the shared native catalog picker.

Opening a discovery row does not automatically download a catalog. Catalog bootstrap starts only when the user requests reading or chapters.

This corresponds to Legado `bookUrl + ruleBookInfo`.

### 3. Catalog

Providers normalize their TOC into the existing file-backed `ChapterCatalogSpec` / FileRows contract.

Large catalogs never become a `std::vector` in UI memory.

This corresponds to Legado `tocUrl + ruleToc`.

### 4. Chapter

Providers implement the existing native chapter adapter and stream/decode directly to the chapter cache.

This corresponds to Legado `chapterUrl + ruleContent`.

## Loading presentation

Provider implementations publish machine states, not site-specific Chinese sentences.

Stable stages:

```text
preparing
resolving
connecting
receiving
processing
writing
ready
auth_required
error
cancelled
```

`M4NativeLoadUi` converts those stages plus bytes/rows/progress/time into the shared e-ink loading presentation.

This keeps discovery, catalog and chapter loading visually consistent.

## M4UI components for novel sources

The common home page uses existing M4UI primitives plus one bounded component:

```xml
<tiles
  source="provider.categories"
  pageSize="8"
  height="116"
  onActivate="provider.selectCategory" />
```

`tiles` is intentionally not a generic grid engine:

- max 8 visible items;
- fixed four-column layout on M4;
- no nested layout;
- no remote images;
- no dynamic CSS;
- one row is materialized at a time.

Recommendation books remain a standard virtual `<list>`.

## Converting a Legado source later

A converter should map the source in four steps:

| Legado concept | M4 concept |
| --- | --- |
| `exploreUrl` | Explore category/query descriptor |
| `ruleExplore` | `BookCard` field projection |
| `bookUrl` + `ruleBookInfo` | `BookDetail` projection |
| `tocUrl` + `ruleToc` | FileRows catalog adapter |
| `chapterUrl` + `ruleContent` | Native chapter adapter |

The converter should **not** translate arbitrary JavaScript into device-side JavaScript. M4 has no browser/JS runtime in the native provider path.

Preferred conversion order:

1. URL templates / query parameters;
2. bounded JSON field paths;
3. bounded HTML extraction helpers;
4. regex/replace helpers;
5. a small provider-native helper for exceptional signing/decryption.

If a Legado source relies on large arbitrary JS programs, DOM mutation or browser execution, it is not automatically portable and needs a dedicated adapter.

## Common-site target

A future declarative source package should be able to describe the majority of conventional novel sites with:

```text
provider identity
headers/cookie policy
explore categories
search endpoint
book-detail endpoint
catalog endpoint
chapter endpoint
field paths
small transforms
encoding
```

The result should still feed the exact same host-owned UI and reader pipeline described above.

## Memory rules

A converted provider must preserve the native memory model:

- response bodies stream to FileRows/SD;
- one record at a time is projected;
- no whole-page DOM;
- no whole-response JSON document for large lists;
- TLS and decode use the existing heavy-resource coordinator;
- UI models remain small and provider-neutral;
- covers are optional/lazy and never required for navigation.

The goal is not full Legado compatibility on-device. The goal is to make **ordinary novel sources cheap to convert while retaining M4's deterministic memory and e-ink UX**.
