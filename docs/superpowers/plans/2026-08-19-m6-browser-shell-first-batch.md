# M6 Browser Shell First Batch Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a usable E-ink browser shell to the existing hidden WebView: omnibox/search resolution, Back/Forward/Home/Tabs/Reload/Menu chrome, and an app-owned English/numeric keyboard for omnibox entry.

**Architecture:** Keep `VirtualBrowserSession` and M4B3 transport untouched except for existing page callbacks. Put non-Android logic in small pure-Java classes under `browser/shell`; make `BrowserPresentation` the 480×800 visual host that delegates URL parsing and keyboard state to those helpers. No third-party dependencies and no M4B3 protocol changes.

**Tech Stack:** Java 11, Android System WebView, `Presentation`, `FrameLayout`/`LinearLayout`/`TextView`/`EditText`, SharedPreferences only where already used, existing no-Gradle `build.sh`.

**Spec:** `docs/superpowers/specs/2026-08-19-m6-browser-shell-ime-design.md`

## Global Constraints

- Authoritative canvas: 480×800 @ 160 dpi.
- Preserve production M4B3 wire semantics.
- No Accessibility/root/ADB/global input as product behavior.
- No system IME dependency for Browser Bridge text entry.
- No animation/translucency/continuous progress repaint.
- Touch targets must be at least 44–48 logical px.
- No firmware flashing in this implementation batch.
- Do not expand m4sim.
- Keep #42 and #44 OPEN until real-device acceptance is complete.

---

### Task 1: Pure-Java omnibox resolver and keyboard state

**Files:**
- Create: `android/m4-screen-bridge/app/src/main/java/com/murphy/m4screenbridge/browser/shell/BrowserAddressResolver.java`
- Create: `android/m4-screen-bridge/app/src/main/java/com/murphy/m4screenbridge/browser/shell/BrowserKeyboardState.java`
- Create: `android/m4-screen-bridge/app/src/test/java/com/murphy/m4screenbridge/BrowserShellLogicTest.java`
- Modify: `android/m4-screen-bridge/build.sh`

**Interfaces:**
- Produces: `BrowserAddressResolver.resolve(String raw, String searchTemplate): String`
- Produces: `BrowserKeyboardState` with `Mode { LETTERS, SYMBOLS }`, `text()`, `append(String)`, `backspace()`, `space()`, `toggleShift()`, `toggleMode()`, `clear()`, `replace(String)`, and `shifted()`.
- Consumes: no Android classes.

- [ ] **Step 1: Write the failing test**

Create `BrowserShellLogicTest.java` with checks equivalent to:

```java
package com.murphy.m4screenbridge;

import com.murphy.m4screenbridge.browser.shell.BrowserAddressResolver;
import com.murphy.m4screenbridge.browser.shell.BrowserKeyboardState;

public final class BrowserShellLogicTest {
    public static void main(String[] args) {
        eq("https://example.com", BrowserAddressResolver.resolve("example.com",
                "https://duckduckgo.com/?q=%s"));
        eq("https://example.com/a", BrowserAddressResolver.resolve("https://example.com/a",
                "https://duckduckgo.com/?q=%s"));
        eq("https://duckduckgo.com/?q=hello+world",
                BrowserAddressResolver.resolve("hello world", "https://duckduckgo.com/?q=%s"));

        BrowserKeyboardState k = new BrowserKeyboardState();
        k.append("a");
        eq("a", k.text());
        k.toggleShift();
        yes(k.shifted());
        k.append("b");
        eq("aB", k.text());
        no(k.shifted()); // one-shot shift
        k.space();
        k.append("c");
        eq("aB c", k.text());
        k.backspace();
        eq("aB ", k.text());
        k.toggleMode();
        eq(BrowserKeyboardState.Mode.SYMBOLS, k.mode());
        k.replace("example.com");
        eq("example.com", k.text());
        System.out.println("BrowserShellLogicTest PASS");
    }

    private static void yes(boolean v) { if (!v) throw new AssertionError(); }
    private static void no(boolean v) { if (v) throw new AssertionError(); }
    private static void eq(Object e, Object a) {
        if (e == null ? a != null : !e.equals(a)) throw new AssertionError("expected=" + e + " actual=" + a);
    }
}
```

- [ ] **Step 2: Run the self-check and verify RED**

Run:

```bash
cd android/m4-screen-bridge
./build.sh --test
```

Expected: compilation failure because `browser.shell.BrowserAddressResolver` / `BrowserKeyboardState` do not exist or are not in the pure-Java source list.

- [ ] **Step 3: Implement `BrowserAddressResolver`**

Required behavior:

```java
public static String resolve(String raw, String searchTemplate)
```

- trim raw input;
- empty → `about:blank`;
- preserve `http://`, `https://`, `about:`, `data:`;
- hostname-like text with no whitespace → prepend `https://`;
- otherwise URL-encode with UTF-8 form encoding and substitute into `%s` in the search template;
- invalid/empty template falls back to `https://duckduckgo.com/?q=%s`.

Hostname-like minimum rule for this batch: no whitespace and either contains `.` or equals `localhost`.

- [ ] **Step 4: Implement `BrowserKeyboardState`**

Use a `StringBuilder`, one-shot Shift, and no Android dependencies. `append(String key)` uppercases alphabetic text under Shift and resets Shift after one append. `backspace()` deletes one Unicode code point, not merely one UTF-16 code unit.

- [ ] **Step 5: Register the helper/test sources in `build.sh` and run GREEN**

Add the two helper Java files and `BrowserShellLogicTest.java` to the `javac --release 11` pure-Java command, then run:

```bash
cd android/m4-screen-bridge
./build.sh --test
```

Expected: existing tests plus `BrowserShellLogicTest PASS`.

- [ ] **Step 6: Commit**

Commit message:

```text
test(m6): add omnibox and keyboard state contracts
```

---

### Task 2: Build the static 480×800 browser shell

**Files:**
- Create: `android/m4-screen-bridge/app/src/main/java/com/murphy/m4screenbridge/browser/shell/BrowserShellStyle.java`
- Modify: `android/m4-screen-bridge/app/src/main/java/com/murphy/m4screenbridge/browser/BrowserPresentation.java`

**Interfaces:**
- Consumes: `BrowserAddressResolver.resolve(...)` from Task 1.
- Produces new `BrowserPresentation` methods: `goForwardInBrowser(): boolean`, `goHomeInBrowser(): boolean`, `focusOmnibox()`, `setHomepage(String)`, `setSearchTemplate(String)`.
- Keeps existing `loadUrl`, `goBackInBrowser`, `reloadBrowser`, `dispatchBrowserTouch` signatures intact.

- [ ] **Step 1: Add compile-visible shell style constants**

`BrowserShellStyle` must define only static constants/helpers used by the Presentation:

```java
public static final int OMNIBOX_HEIGHT = 52;
public static final int TOOLBAR_HEIGHT = 56;
public static final int TOUCH_MIN = 48;
public static final int BLACK = Color.BLACK;
public static final int WHITE = Color.WHITE;
```

Keep dimensions in logical pixels because the Presentation is explicitly 160 dpi.

- [ ] **Step 2: Replace the full-screen WebView layout with three fixed regions**

In `BrowserPresentation.onCreate()` build an opaque vertical shell:

```text
52 px omnibox row
MATCH_PARENT weighted WebView host
56 px bottom toolbar
```

Use Android framework widgets only. The WebView must remain the same owned WebView object and still receive existing M4B3 touch dispatch.

- [ ] **Step 3: Implement the omnibox row**

Use an `EditText` configured as a single-line URL/search field with system soft input suppressed:

```java
omnibox.setShowSoftInputOnFocus(false);
omnibox.setSingleLine(true);
```

On editor action / explicit keyboard Go:

```java
String resolved = BrowserAddressResolver.resolve(
        omnibox.getText().toString(), searchTemplate);
webView.loadUrl(resolved);
```

While the user is editing, page callbacks must not overwrite the field. On focus loss or navigation completion, sync it to the current page URL.

- [ ] **Step 4: Implement the six-button toolbar**

Create six equal-width static `TextView` or `Button` targets:

```text
← | → | ⌂ | Tabs | ↻ | ⋮
```

Actions:

```java
Back    -> goBackInBrowser()
Forward -> goForwardInBrowser()
Home    -> goHomeInBrowser()
Tabs    -> toggleTabsPanel()
Reload  -> reloadBrowser()
Menu    -> toggleMenuPanel()
```

No ripple/animation dependency is required. Use black border/background changes only if needed for press feedback.

- [ ] **Step 5: Add static Tabs/Menu scaffolds**

For this first batch, the panels are real shell surfaces but only expose implemented commands:

Tabs panel:

```text
Current tab
+ New tab (may initially create/load home in the single WebView and close panel)
Close panel
```

Menu panel:

```text
Home
Focus address
Reload
Close menu
```

Do not label bookmarks/history/downloads as completed yet.

- [ ] **Step 6: Update page callbacks**

`onPageStarted`/`onPageFinished` must update internal `currentPageUrl`; only update omnibox text when it is not actively focused/being edited. `onReceivedTitle` may remain listener-only for now.

- [ ] **Step 7: Build the Android APK**

Run:

```bash
cd android/m4-screen-bridge
./build.sh --test
./build.sh
```

Expected: all pure-Java tests PASS and debug APK builds/signs successfully.

- [ ] **Step 8: Commit**

Commit message:

```text
feat(m6): add E-ink browser shell chrome
```

---

### Task 3: Add app-owned omnibox keyboard

**Files:**
- Create: `android/m4-screen-bridge/app/src/main/java/com/murphy/m4screenbridge/browser/shell/M4KeyboardView.java`
- Modify: `android/m4-screen-bridge/app/src/main/java/com/murphy/m4screenbridge/browser/BrowserPresentation.java`

**Interfaces:**
- Consumes: `BrowserKeyboardState` from Task 1.
- `M4KeyboardView.Listener`:

```java
interface Listener {
    void onTextChanged(String text);
    void onSubmit(String text);
    void onHideRequested();
}
```

- `M4KeyboardView.showForText(String initialText)` and `hideKeyboard()`.

- [ ] **Step 1: Implement the keyboard as a fixed Android View**

Use `LinearLayout`/`TextView` keys; no system IME, no animations, no third-party library.

Letters layout:

```text
q w e r t y u i o p
 a s d f g h j k l
⇧ z x c v b n m ⌫
123   Space   Go   Hide
```

Symbols layout should provide at least:

```text
1 2 3 4 5 6 7 8 9 0
. / : - _ ? & = % +
ABC   Space   Go   Hide
```

Every key target height must be >=48 logical px.

- [ ] **Step 2: Bind keyboard state to omnibox text**

On omnibox focus/tap:

```java
keyboard.showForText(omnibox.getText().toString());
```

Each key mutation updates the omnibox through `onTextChanged`. Do not ask Android to show the system keyboard.

- [ ] **Step 3: Submit through the same resolver**

`onSubmit(text)` resolves with `BrowserAddressResolver`, loads the URL, clears omnibox editing state and hides the app keyboard.

- [ ] **Step 4: Resize the page discretely**

Keyboard visibility changes layout once per show/hide. No translation/alpha animation. The fixed bottom toolbar may be hidden while the keyboard is visible so the keyboard gets enough vertical area; the omnibox remains visible.

- [ ] **Step 5: Preserve M4 touch dispatch across shell and WebView**

`dispatchBrowserTouch(MotionEvent)` currently sends events directly to the WebView, which would bypass shell controls. Change the Presentation-level dispatch target to the root shell container so M4 coordinates can hit omnibox, keyboard and toolbar as normal views:

```java
return root != null && event != null && root.dispatchTouchEvent(event);
```

Keep a direct WebView helper only if deterministic lab tests still require it. This is a critical product change and must be compiled before device testing.

- [ ] **Step 6: Build and run all Android self-checks**

Run:

```bash
cd android/m4-screen-bridge
./build.sh --test
./build.sh
```

Expected: PASS and signed debug APK.

- [ ] **Step 7: Commit**

Commit message:

```text
feat(m6): add app-owned omnibox keyboard
```

---

### Task 4: CI artifact and Motorola UI smoke

**Files:**
- Modify only if required by CI trigger: `.github/workflows/android-screen-bridge.yml`
- No firmware files.

**Interfaces:**
- Consumes the debug APK produced by Tasks 1–3.
- Produces real-device screenshots/dumpsys/log evidence; no code API.

- [ ] **Step 1: Push branch and trigger existing Android Screen Bridge workflow**

The workflow must run:

```text
Pure Java self-checks
Build debug APK
Upload debug APK
```

Do not alter CI semantics merely to make a failure green.

- [ ] **Step 2: Download the exact-head artifact**

Record workflow run id, head SHA, artifact id and SHA-256/digest.

- [ ] **Step 3: Install on authorized Motorola `ZY22KN7WSK`**

Use the exact CI artifact. If the debug signature conflicts with the installed package, uninstall/reinstall is allowed for this app only; do not modify unrelated system state.

- [ ] **Step 4: Run an Android-side shell smoke before M4 firmware work**

Verify on the 480×800 Presentation capture/diagnostics:

```text
omnibox visible
six bottom actions visible
address tap shows app-owned keyboard
letters/symbols/backspace/space mutate omnibox
a Go submission navigates WebView
Back/Forward/Home/Reload respond
Tabs/Menu panels open/close
```

- [ ] **Step 5: Re-check transport invariants**

While using the shell, ensure M4B3 connection diagnostics remain live and no new `CONNECTED + CRC0` zombie behavior is introduced. This is regression evidence, not a reason to delay shell UI implementation before testing it.

- [ ] **Step 6: Update Issue #44**

Post exact branch/head, commits, CI run/artifact, implemented commands, known limitations (general webpage text fields and Chinese pinyin still pending), and Motorola evidence. Keep #44 OPEN.

- [ ] **Step 7: Commit any evidence-only doc update if needed**

No completion claim until the real M4 can operate the browser from its touch panel.
