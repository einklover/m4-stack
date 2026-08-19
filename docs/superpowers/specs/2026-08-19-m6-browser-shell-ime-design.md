# M6 Browser Shell + App-Owned IME Design

**Issue:** #44 — M6 Browser Shell + app-owned IME inspired by Via  
**Branch:** `agent/eink-browser-m6-shell-ime`  
**Base:** `f3d8809500a50c602ed207a3a2103f1891428548` from `agent/eink-browser-bridge-m5-productization`

## Goal

Turn the existing Android-owned hidden `WebView`/`VirtualDisplay` Browser Bridge into a practical 480×800 E-ink browser that can be operated from the real Murphy M4 while the phone screen stays off. Android owns browser logic and text input; M4 stays a thin display/touch terminal. The production M4B3 wire protocol remains unchanged for browser-shell actions that can be represented as normal touch.

## Reference implementations

### EinkBro — primary implementation reference

EinkBro is the closest architectural/product match because it is an Android WebView browser explicitly optimized for E-ink. Useful patterns to borrow conceptually:

- split browser responsibilities into focused controllers rather than one monolithic Activity;
- explicit tab container lifecycle;
- detach a `WebView` before `destroy()` so closed tabs are not pinned by their parent view;
- browser actions such as forward/back, refresh, page up/down, bookmarks/history, find/search and touch pagination are first-class browser commands;
- E-ink UX favors static state transitions rather than animation-heavy chrome.

We will not copy EinkBro's Kotlin architecture wholesale. The M4 Browser Bridge has a much smaller Java-only, no-Gradle-runtime-dependency build and must preserve its existing `Presentation`/`VirtualDisplay` ownership model.

### Fulguris — feature-completeness reference

Fulguris is used as a checklist for normal browser product expectations: sessions/tabs, address bar, bookmarks, history, incognito, search engines, desktop rendering and settings. It is not used as an architecture template because its product surface is much larger than required for M6.1/M6.2.

### Via — UX reference only

Via remains a useful compact-browser product reference. Its public repository does not expose the complete browser implementation used by the app, so M6 must not depend on copying internal Via source.

### Lightning / Jelly — complexity guardrails

Lightweight WebView browser projects are useful as a reminder to delegate rendering, history primitives and navigation semantics to Android WebView where possible. M6 should avoid recreating a browser engine or large framework layer.

## Product architecture

```text
BrowserBridgeService
        |
VirtualBrowserSession
        |
BrowserPresentation (480x800 app-owned Presentation)
        |
        +-- BrowserShellController
        |      +-- omnibox/navigation state
        |      +-- home/menu state
        |      +-- active-tab commands
        |
        +-- BrowserTabStore
        |      +-- BrowserTabState[]
        |      +-- active tab id
        |      +-- persistence snapshot
        |
        +-- WebView host
        |
        +-- M4KeyboardView
               +-- English
               +-- numeric/symbol
               +-- later: Chinese pinyin candidate engine
```

`VirtualBrowserSession` remains responsible for capture, M4B3 transport, connection diagnostics and touch dispatch. It must not become the owner of browser tabs, bookmarks, keyboard layout or search semantics.

`BrowserPresentation` becomes the visual host for the browser shell and keyboard but delegates state transitions to focused helper classes so the file does not become a full browser application controller.

## 480×800 shell geometry

Authoritative logical canvas: **480×800 @ 160 dpi**.

Normal browsing layout:

```text
0..51      omnibox row (52 px)
52..743    active WebView viewport (692 px)
744..799   navigation toolbar (56 px)
```

Bottom toolbar actions in M6.1:

```text
Back | Forward | Home | Tabs | Reload | Menu
```

Six equal-width targets are approximately 80×56 logical pixels, comfortably above the 44–48 px minimum touch target.

When the app-owned keyboard is visible, it occupies a fixed bottom region above or in place of the normal navigation toolbar. The WebView viewport is resized discretely; it must not animate during keyboard show/hide.

## E-ink rendering rules

- black/white opaque surfaces only for the first implementation;
- no alpha/translucency blur;
- no animated progress indicator or continuous spinner;
- no toolbar hide/show animation tied to every scroll event;
- menu surfaces are static full-width panels or simple dialogs;
- progress is represented by coarse text/state updates rather than continuously repainting a bar;
- stable keyboard geometry so each key press dirties a small predictable region;
- preserve the current FAST-only Browser Bridge presenter policy and all #33/#34 physical-baseline invariants.

## M6.1 Browser Shell

### Omnibox

The top row shows the current URL or editable address/search text. On submit:

1. trim whitespace;
2. if input already has a supported URL scheme (`http`, `https`, `about`, `data`) load it directly;
3. if text resembles a hostname, prepend `https://`;
4. otherwise build a search URL using the configured search engine template.

Initial search engine: DuckDuckGo (`https://duckduckgo.com/?q=%s`). Search-engine configurability is represented in state from the start, even if UI selection arrives in a later M6 task.

### Navigation

- Back: `WebView.canGoBack()` → `goBack()`.
- Forward: `WebView.canGoForward()` → `goForward()`.
- Reload: `WebView.reload()`.
- Home: load configured homepage; initial homepage is a deterministic local start page or `about:blank` plus omnibox focus.
- Tabs: open the static tab panel.
- Menu: open a static browser menu panel.

The existing hardware-key M4B3 path may continue to map physical Back/Reload to the active WebView, but shell buttons use ordinary M4 touch and do not require protocol additions.

### Page callbacks

`onPageStarted`, `onPageFinished`, title and progress callbacks update shell state. UI updates must be idempotent: the omnibox text is not overwritten while the user is editing it.

## M6.2 App-owned keyboard

### Why it is required

The browser lives on an app-owned `VirtualDisplay`; relying on the system IME to appear on that display is not a valid product path. The keyboard therefore lives inside the same `Presentation` and is captured into M4 frames like every other browser-shell element.

### First keyboard modes

- English lower-case letters;
- English upper-case via a discrete Shift state;
- numeric/symbol layout;
- Space;
- Backspace;
- Enter/Go;
- keyboard hide.

### Text targets

Two target classes share one keyboard engine:

1. **Omnibox target** — direct Java string editing; submit triggers navigation/search.
2. **WebView target** — commit to the focused page control using WebView's focused editor/InputConnection path where available.

The first device milestone must support normal `<input>` and `<textarea>` fields. Contenteditable and difficult JS editors are explicitly a later compatibility layer if the normal editor path is insufficient.

### Focus detection

The shell must detect when a WebView editor becomes active and show the app-owned keyboard without invoking a system soft keyboard on the phone display. Keyboard show/hide is a discrete layout change.

### Password/privacy behavior

- never persist password-field text;
- no candidate/history storage for password fields;
- future Chinese candidate engine receives a `sensitive` flag and must suppress learning/history;
- incognito state must use the same non-persistence rules.

## Tab model

Tabs are Android-side objects. M4B3 does not know about them.

`BrowserTabState` contains only persistable metadata:

```text
id
url
title
privateMode
```

The runtime tab object owns a WebView. Exactly one active tab is attached to the visible WebView host. Closed tabs are detached before `WebView.destroy()`, following the lifecycle lesson from EinkBro's container implementation.

M6.1 may ship with a single active tab plus a tab-panel scaffold; full create/close/switch/restore behavior follows immediately after shell/keyboard viability is proven.

## Persistence

Persist non-sensitive browser state with the existing Android SharedPreferences approach unless storage size proves insufficient:

- homepage;
- search template;
- last normal tabs and selected tab;
- bookmarks/history later;
- last URL remains compatible with the existing BrowserBridgeService resume path.

Never persist:

- incognito tabs/history;
- password contents;
- keyboard composition/candidates;
- transient connection error overlays.

## Browser menu roadmap

M6.1 menu scaffold exposes only commands that already have implementation or a deterministic state stub. The full M6 essentials sequence is:

1. Find in page.
2. Bookmark current page / bookmarks list.
3. History.
4. Desktop-site toggle / UA policy.
5. JavaScript toggle.
6. Image loading toggle.
7. Text size/force zoom.
8. Downloads/file chooser.
9. Long-press link/image context actions.
10. Incognito/private tabs.
11. Optional user CSS/JS hooks.

Avoid dead buttons labelled as finished features.

## Transport/lifecycle boundaries

- Browser shell state survives ordinary M4 TCP reconnects.
- A transport disconnect must not destroy the active WebView or keyboard state.
- Product `CONNECTED` remains gated on M4B3 HELLO_OK as established by the M5 reconnect fix.
- The HELLO watchdog remains transport-only and cannot reset browser tabs.
- Reconnect emits a fresh keyframe from the still-live browser presentation.
- MainActivity recreation must not own or destroy the browser session.

## Error handling

- Navigation failure is rendered as a high-contrast browser error state while preserving the omnibox and navigation controls.
- Invalid omnibox text becomes a search query rather than an exception.
- WebView editor commitment failure leaves keyboard visible and reports a debug diagnostic; it must not crash the session.
- A closed/destroyed tab cannot remain active; tab store selects an adjacent surviving tab or creates a home tab.
- connection overlay remains above page content but must not permanently cover shell controls after recovery.

## Testing strategy

### Pure Java tests

Add tests for logic that does not require Android:

- omnibox classification and search URL generation;
- tab-state add/select/close transitions;
- keyboard text-buffer editing and Shift/mode transitions;
- persistence serialization helpers if introduced.

These run under existing `./build.sh --test`.

### Android compile gate

`android/m4-screen-bridge/build.sh` must continue producing the debug APK without introducing a Gradle/Maven dependency requirement.

### Emulator/device UI validation

Emulator is preferred for rapid shell/keyboard interaction once `emulator-5554` is available; use `adb reverse` for debug-side-channel tooling where needed.

Real-device acceptance uses Motorola `ZY22KN7WSK` plus real M4:

1. open/focus omnibox from M4 touch;
2. type URL/search using the M4-visible app keyboard;
3. navigate Back/Forward/Home/Reload;
4. tap ordinary webpage links and scroll;
5. focus real webpage input/textarea and enter text;
6. keep phone physical screen off for at least 90 seconds and continue browsing;
7. disconnect/reconnect M4 transport and verify browser/tab/input state survives and a fresh framebuffer resumes;
8. no white-screen, crash, WDT or presenter/ACK regression.

## Non-goals for the first implementation batch

- no browser engine replacement (no GeckoView/Chromium embedding project);
- no system/global IME integration;
- no Accessibility/root/ADB input as product behavior;
- no M4B3 browser-command extensions;
- no multitouch;
- no firmware flashing as part of shell implementation;
- no animation framework;
- no large third-party Android dependencies.

## First implementation batch

The first code batch after this design is deliberately narrow and device-visible:

1. omnibox parsing/state helper;
2. top omnibox UI;
3. bottom Back/Forward/Home/Tabs/Reload/Menu toolbar;
4. forward/home support in `BrowserPresentation`;
5. static menu/tab-panel scaffolds;
6. app-owned English/numeric keyboard for omnibox;
7. pure-Java tests + Android debug APK;
8. inspect the actual 480×800 output before expanding the keyboard into general WebView editors and Chinese pinyin.
