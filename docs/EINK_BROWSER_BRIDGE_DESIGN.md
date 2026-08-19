# E-ink Browser Bridge design

Status: M0–M4 implemented on device; #34 optical PASS CLOSED at `8ff3285`; #39 LAN AUTO discovery CLOSED at `57527e8`  
Current implementation branch: `agent/eink-browser-bridge-discovery` (`57527e8`, merge of `a7aee33` + `8ff3285`)  
Target: Murphy M4 + Android `m4-screen-bridge`

## 1. Goal

Add a browser mode that makes Murphy M4 behave like a thin e-ink terminal for a full Android-hosted web browser:

- the Android phone owns all browser/network/JavaScript work;
- the browser renders at the M4 logical resolution instead of the phone panel resolution;
- the phone sends only changed regions whenever practical;
- the M4 applies those regions with e-ink-aware partial refresh;
- M4 touch/gesture/key events are returned to the Android browser;
- the physical phone display should not be part of the rendering pipeline and the design should be able to continue while the phone screen is off, subject to real-device Android/OEM validation;
- Wi-Fi is the primary framebuffer transport; Bluetooth is an optional control / low-bandwidth fallback path.

This is intentionally **not** a generic remote-desktop protocol. The phone is the browser computer and M4 is an e-ink browser terminal.

## 2. Existing code we should reuse

`android/m4-screen-bridge` already provides useful pieces:

- fixed phone-side server on TCP port `48624`;
- M4 logical portrait coordinates `480x800` and physical panel packing `800x480`;
- 1bpp framebuffer generation;
- RLE1 codec;
- bounded page storage;
- touch forwarding;
- Wi-Fi phone discovery conventions;
- pure-Java protocol/packing tests.

The current realtime path is based on `AccessibilityService.takeScreenshot()`. That path necessarily depends on the physical Android screen and currently documents that the phone must remain on/unlocked. Browser Bridge should **not** extend that screenshot architecture. It should add a new rendering backend that owns its own browser surface.

The old accessibility/XHS/Fanqie paths remain valid and should not be regressed.

## 3. Recommended architecture

```text
                     Android phone

       +------------------------------------------+
       | M4 Screen Bridge foreground service      |
       |                                          |
M4 --> | BridgeProtocolV3 <----> BrowserSession   |
input  |      ^                    |               |
       |      |                    v               |
       |      |            VirtualDisplay         |
       |      |              480 x 800             |
       |      |                    |               |
       |      |              Presentation         |
       |      |                    |               |
       |      |        Browser chrome + WebView   |
       |      |            (Chromium engine)      |
       |      |                    |               |
       |      |                    v               |
       |      +---- FrameDiffer <- ImageReader    |
       |             |                            |
       |             v                            |
       |       EInkQuantizer / RectMerger          |
       |             |                            |
       |             v                            |
       |         patch encoder                     |
       +-------------|----------------------------+
                     |
              Wi-Fi persistent TCP
                     |
                     v
       +------------------------------------------+
       | Murphy M4                               |
       | BrowserBridgeClient                     |
       |   -> receive patch                      |
       |   -> update PSRAM framebuffer           |
       |   -> partial/full panel refresh         |
       |   -> ACK applied frame                  |
       |   <- touch/drag/key/browser commands    |
       +------------------------------------------+
```

### 3.1 Android virtual display

Create an app-owned private `VirtualDisplay` using `DisplayManager.createVirtualDisplay()`.

Proposed initial metrics:

- logical width: `480`;
- logical height: `800`;
- density: start around `160 dpi`, then make configurable after visual testing;
- flags: `VIRTUAL_DISPLAY_FLAG_PRESENTATION | VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY`;
- output surface: `ImageReader.getSurface()`;
- image format: RGBA8888 for the first implementation.

Android documents that a virtual display renders its content into the `Surface` supplied by the application. This gives us a framebuffer source independent of the physical phone panel.

Official references:

- https://developer.android.com/reference/android/hardware/display/DisplayManager
- https://developer.android.com/reference/android/media/ImageReader
- https://developer.android.com/reference/android/hardware/display/VirtualDisplay

### 3.2 Browser window on the virtual display

Attach a `Presentation` to the `VirtualDisplay` and build the browser UI inside the presentation context.

`Presentation` is preferable to trying to launch an external browser Activity onto an artificial display because:

- it is designed to host application content on another display;
- the Screen Bridge app owns the complete lifecycle;
- exact display metrics are under our control;
- input can be delivered directly to the owned view tree;
- browser chrome and page output are captured in one framebuffer;
- no `MediaProjection` screen-capture permission loop is required for the browser backend.

Official reference:

- https://developer.android.com/reference/android/app/Presentation

### 3.3 Browser engine

First implementation should use Android System WebView, not an external Chrome process.

Reason: WebView is the Android Chromium-based web-content engine and gives the bridge direct ownership of the rendered view. That is much more suitable for an e-ink transport than remote-controlling another app.

Browser shell responsibilities:

- address bar;
- back / forward / reload / stop;
- home/new tab/tab switcher;
- JavaScript enabled;
- DOM storage / cookies;
- normal navigation and redirects;
- `WebChromeClient` support for JavaScript dialogs, permissions, window creation and file chooser where practical;
- downloads delegated to Android `DownloadManager`;
- geolocation/permission prompts rendered as bridge-owned e-ink-friendly dialogs;
- multiple tabs with a configurable bound so a large tab set cannot grow without limit;
- persistent cookies/history/bookmarks on the phone.

WebView is not literally the Chrome application. Chrome extensions and some Chrome-only browser UI/features are out of scope. If a future requirement truly needs the external Chrome app, add a second backend later; do not make it the foundation of v1.

Official reference:

- https://developer.android.com/develop/ui/views/layout/webapps
- https://developer.android.com/reference/android/webkit/WebView

## 4. Frame capture and damage detection

Do **not** transmit every RGBA frame.

The Android compositor does not expose a stable public WebView damage-rectangle API suitable for this protocol. Therefore the bridge should derive dirty regions itself.

### 4.1 Pipeline

For every useful `ImageReader` frame:

1. acquire the newest image; drop obsolete queued images;
2. convert only as much as needed to grayscale / target e-ink representation;
3. compare against the last acknowledged browser framebuffer by tiles;
4. mark changed tiles;
5. merge neighboring tiles into rectangles;
6. discard tiny/noise-only changes under a configurable threshold;
7. encode each rectangle;
8. send a `FRAME_PATCH` referencing the framebuffer version it is based on;
9. M4 applies it and returns `FRAME_ACK`;
10. phone advances the acknowledged base only after the ACK.

Initial tile size: `16x16` or `32x16`; benchmark both. Text-heavy pages normally create highly local changes, so this should be much cheaper than full-frame streaming.

### 4.2 Keyframes and recovery

Patch frames must contain:

- `frame_id`;
- `base_frame_id`;
- rectangle list;
- pixel mode / codec;
- CRC32 of the resulting logical framebuffer, or another inexpensive integrity check.

If:

- the M4 reconnects;
- an ACK times out;
- `base_frame_id` mismatches;
- CRC validation fails;

then send a full keyframe and restart the patch chain.

Never make correctness depend on TCP delivery alone; explicit frame identity protects us from reconnect/restart state divergence.

## 5. E-ink rendering modes

A browser needs two qualitatively different update behaviours.

### 5.1 Interactive mode

During touch, drag, scroll, caret movement or rapidly changing content:

- prefer 1bpp or low-gray output;
- aggressive dirty-rect merging;
- fast partial refresh waveform;
- cap update rate to what the panel can actually display;
- latest-frame-wins: obsolete intermediate browser frames may be dropped.

The goal is responsiveness, not photographic quality.

### 5.2 Settled mode

After an idle debounce (initial proposal: roughly `300-600 ms`, tune on hardware):

- recompute higher-quality grayscale/dithered output;
- send the final changed regions;
- use the panel's better-quality refresh path where available;
- optionally do a larger cleanup refresh after long scrolling sessions.

This gives fast interaction followed by a clean stable page.

### 5.3 Ghosting budget

Maintain counters such as:

- partial update count;
- cumulative changed pixels;
- high-contrast inversion count;
- elapsed time since cleanup.

Trigger a full/cleanup refresh when a threshold is crossed rather than blindly doing a full refresh every N frames.

Browser Bridge production policy (M4PanelDirty): unique tile coverage and transition churn drive a stock HALF hygiene clean from the frozen present snapshot. A fixed tiny-Partial count is not a sole FULL trigger. FirstBaseline / Untrusted / DenseArea / Fragmented / reconnect recovery stay absolute FULL. Do not invent custom LUT/voltage/waveform.

Exact waveform selection must follow the M4's existing display driver capabilities; this design must not invent unsupported grayscale/partial modes.

## 6. Transport

### 6.1 Wi-Fi: primary

Use Wi-Fi for framebuffer data.

Keep the existing discovery/phone addressing convention and port `48624`, but introduce a v3 persistent duplex stream, for example:

```text
GET /v3/browser/stream HTTP/1.1
Connection: Upgrade
Upgrade: m4-browser-v3

HTTP/1.1 101 Switching Protocols
...
<binary framed duplex protocol>
```

A raw second port is simpler internally, but reusing `48624` is preferable for discovery, firewall behaviour and one-service ownership.

### 6.1.1 LAN discovery (mDNS / DNS-SD)

The Browser Bridge TCP endpoint is advertised and discovered on the local LAN so the Android client does not have to type the M4 IPv4 address. BLE is not used.

**Service record** (existing ESP32 `ESPmDNS` / `mdns.h` + Android `NsdManager` / DNS-SD; no extra libraries):

```text
type     _m4b3._tcp.local
name     murphy-m4-browser
host     murphy-m4.local  (STA IPv4 A record)
port     48624
TXT      proto=m4b3
         role=browser-bridge
```

Firmware (`M4B3DiscoveryAdvertise`) adds this service only while the M4B3 listener is bound on a real STA IPv4. It never calls `MDNS.end()`, so keyboard/web responders can keep the shared hostname. Framing, ACK, display, and input are unchanged.

Android (`M4LanDiscovery` + `NsdM4Discovery`) classifies `m4b3_host` and selects an endpoint with this precedence:

1. **manual** — non-empty `m4b3_host` that is not `loopback`/`local`. Discovery does not start. Invalid host/port → no-host.
2. **discovered** — live `_m4b3._tcp` records, validated (IPv4/hostname, port 1–65535, proto TXT empty or `m4b3`), deduplicated by `host:port`, then lexicographically smallest host/port/name.
3. **cached** — last successfully discovered endpoint, used while searching and after live loss (unless that same host is reported lost).
4. **none** — empty host, no live/cache. Capture continues; frames are skipped until an endpoint appears.
5. **loopback** — explicit `loopback`/`local` bypasses discovery and stays on the in-process reference receiver.

Discovery is lifecycle-bounded: one `NsdManager` listener, one in-flight resolve, queue/candidate cap 8, at most 3 discover restarts, 8 s search timeout. `VirtualBrowserSession.stop()` drops the listener, multicast lock, and engine. Status is exposed on the session snapshot (`src=`, `phase=`, `ep=`, retries/errors) without a UI redesign.

### 6.1.2 Staged heads and remaining real-device proof

Host-only discovery landed at `f0cfeb3` / `a7aee33`. #34 optical PASS closed at `8ff3285`. Those heads diverged after `821acd8` and must stay ancestors of the discovery branch via a normal merge.

| Stage | Head | Meaning |
|-------|------|---------|
| #34 nearby-window merge | `821acd8b71464032304b1d90e1ca27c29a7d8320` | Sparse HUD/glyph windows stay Partial. Ancestor of both later heads. |
| #34 hygiene (CLOSED) | `8ff32859cb52dd82a6fdc4990337ff5988a69ee5` | Content-aware unique coverage / transition churn drives stock HALF hygiene. Count-8 is not a sole FULL trigger. User optical PASS. |
| #38 host soak | `bd23f7317c132bde937abe7eb0ee2d11970d8af1` | Deterministic merge-boundary / ACK / presentBuf host tests. |
| #39 discovery host | `f0cfeb3caf7881c31904a3b8a253918854ac33eb` / `a7aee3315c129c2d48bf91c03436825b6fc05e21` | `_m4b3._tcp` advertise + Android DNS-SD selection. |

Do not merge discovery to `main` until real-device AUTO discovery / reconnect evidence exists. Firmware flashes stay APP1-only @ `0x6e0000`, hash-verified slot 1.

Binary packet envelope proposal:

```text
magic       4 bytes   "M4B3"
type        u8
flags       u8
header_len  u16 LE
payload_len u32 LE
seq         u32 LE
header      variable
payload     variable
```

Required message types:

```text
HELLO
HELLO_ACK
FRAME_KEY
FRAME_PATCH
FRAME_ACK
TOUCH
GESTURE
KEY
TEXT
BROWSER_CMD
BROWSER_STATE
PING
PONG
ERROR
```

### 6.2 Codec

Start with codecs that are cheap on ESP32-S3:

- raw 1bpp/2bpp patch;
- existing RLE1 for binary regions;
- PackBits/RLE-style grayscale codec if measurements justify it.

Do not put PNG/JPEG decoding in the critical M4 patch path unless measurement proves it is better. The phone has CPU to spare; the M4 path should remain bounded and predictable.

### 6.3 Bluetooth

Bluetooth should not be the primary v1 framebuffer transport.

BLE GATT throughput is too device-dependent for comfortable full-page browser frames. Proposed roles:

- phone discovery/pairing;
- wake/control channel;
- emergency 1bpp low-rate fallback;
- optional future L2CAP/optimized BLE transport after measurement.

v1 acceptance should be based on Wi-Fi. A Bluetooth-only transport can be a later milestone rather than delaying the browser architecture.

## 7. Input model

Because the bridge owns the WebView, browser input should no longer use Accessibility global gestures.

### 7.1 Pointer input

M4 sends logical portrait coordinates directly:

```text
x: 0..479
y: 0..799
```

Android injects them into the presentation/browser view tree on the UI thread.

Support at least:

- pointer down;
- pointer move;
- pointer up;
- tap;
- long press;
- drag/scroll;
- double tap.

If M4 hardware exposes reliable multi-touch later, add pointer IDs and pinch. Do not make v1 depend on multi-touch; browser zoom can also be exposed through commands/buttons.

### 7.2 Browser commands

Dedicated commands are better than trying to hit tiny toolbar targets for every action:

```text
BACK
FORWARD
RELOAD
STOP
HOME
FOCUS_ADDRESS
NEW_TAB
CLOSE_TAB
NEXT_TAB
PREV_TAB
ZOOM_IN
ZOOM_OUT
FIND_IN_PAGE
```

The M4 UI can map these to toolbar buttons, gestures, or hardware keys.

### 7.3 Text input

Text entry is a first-class requirement, not an afterthought.

Protocol must support:

- UTF-8 text commit;
- backspace/delete;
- enter;
- tab;
- escape;
- cursor left/right where possible.

Recommended v1 UX:

1. M4 taps the address bar or a page input;
2. M4 opens its native soft keyboard / text-entry overlay;
3. text is committed to Android with `TEXT` messages;
4. Android sends the next rendered patch.

This avoids depending on the Android physical-screen IME. The Android side should implement a dedicated text-injection adapter for WebView/form focus; arbitrary Unicode, including Chinese, must be included in acceptance tests.

## 8. Browser-specific e-ink adaptations

Do not rewrite arbitrary websites. The browser must work with normal pages first.

Optional bridge/browser settings:

- default page zoom;
- force light background for dark-only sites when safe;
- user-selectable contrast curve;
- image quality: off / binary / grayscale;
- animation suppression mode via injected CSS;
- reduce-motion preference;
- hide video frames while allowing audio playback on the phone;
- reader-mode experiment later, separate from normal browsing.

Animation suppression should be opt-in because some sites depend on animated transforms for navigation.

## 9. Phone screen-off behaviour

This design deliberately removes physical-screen capture from the browser path. The target runtime is:

- Screen Bridge foreground service alive;
- app-owned `VirtualDisplay` alive;
- `Presentation` + WebView alive on that virtual display;
- `ImageReader` consuming the virtual framebuffer;
- `PARTIAL_WAKE_LOCK` only if required to keep CPU/network work alive;
- physical phone display allowed to turn off.

However, Android/WebView lifecycle and OEM power policies can still throttle or suspend off-screen rendering. Therefore **screen-off support is a real-device acceptance item, not something to claim from API shape alone**.

Test at minimum:

- AOSP-like device;
- the user's primary Android phone;
- 30 minutes physical screen off;
- JavaScript timers/network/navigation still functional;
- frame callbacks resume on remote M4 interaction;
- no unexpected physical screen wake.

If an OEM throttles the presentation/WebView, investigate lifecycle/foreground-service handling before falling back to keeping the physical panel on.

## 10. Proposed Android code layout

Extend the existing app rather than creating another APK:

```text
android/m4-screen-bridge/app/src/main/java/com/murphy/m4screenbridge/
  browser/
    BrowserSession.java
    BrowserPresentation.java
    BrowserController.java
    BrowserWebViewClient.java
    BrowserChromeClient.java
    BrowserInput.java
    VirtualBrowserDisplay.java
    BrowserStateStore.java
  stream/
    BridgeProtocolV3.java
    BrowserStreamSession.java
    FrameDiffer.java
    RectMerger.java
    EInkQuantizer.java
    PatchCodec.java
```

Existing classes such as `HttpServer`, `Framebuffer`, `Rle`, `Prefs` and discovery helpers should be reused or generalized where that reduces duplication.

Do not mix browser capture logic into the existing accessibility page-prefetch state machine.

## 11. Proposed M4 code shape

Exact paths should be chosen after inspecting the current UI/app conventions, but keep responsibilities separate:

```text
BrowserBridgeClient
  - discovery/connect/reconnect
  - protocol parser
  - keyframe/patch application
  - ACK/resync

BrowserFramebuffer
  - bounded PSRAM-backed framebuffer
  - rectangle writes
  - pixel-format conversion if needed

BrowserInputController
  - touchscreen -> TOUCH/GESTURE
  - browser commands
  - text entry

BrowserActivity / BrowserView
  - Home launcher entry
  - connection state
  - toolbar/soft keyboard hooks
  - refresh policy integration
```

Memory rule: never buffer an unbounded sequence of frames or patches. Keep one current framebuffer plus bounded receive/decode scratch storage.

## 12. State machine

```text
DISCONNECTED
  -> DISCOVERING
  -> CONNECTING
  -> HANDSHAKE
  -> SYNCING (expect FRAME_KEY)
  -> ACTIVE
       | input -> phone
       | patch -> apply -> refresh -> ACK
       | idle -> settled-quality patch
       | timeout/base mismatch -> RESYNC
  -> DISCONNECTED
```

Phone browser session should survive a short M4 disconnect. Reconnecting M4 requests a fresh keyframe and continues the same tab/session.

## 13. Security

The bridge is effectively a remote browser-control endpoint, so do not leave it as an unauthenticated LAN service.

Before feature completion add:

- one-time pairing code shown on phone and entered/confirmed on M4;
- persisted device key/token;
- reject unknown clients;
- bind browser control to the active paired device/session;
- explicit user-visible Start/Stop browser bridge control;
- no arbitrary remote URL/open command before authentication.

TLS may be unnecessary on a trusted local link if pairing plus message authentication is implemented, but the threat model should be decided before release rather than implicitly trusting port `48624`.

## 14. Milestones

### M0 — design spike

- create this document;
- prove `VirtualDisplay -> Presentation -> WebView -> ImageReader` on one Android device;
- prove exact `480x800` browser output;
- verify rendering while physical screen is off.

No firmware change required for the first spike; dump captured frames on Android first.

### M1 — one-way browser framebuffer

- Android WebView browser shell;
- full keyframe over Wi-Fi;
- M4 displays browser frame;
- reconnect sends keyframe.

### M2 — touch and navigation

- tap/down/move/up;
- drag scrolling;
- back/forward/reload;
- address bar input;
- page form text input.

### M3 — dirty rectangles

- tile hashes/diff;
- rectangle merge;
- patch protocol;
- ACK/base-frame recovery;
- bandwidth and latency measurements.

### M4 — e-ink interaction quality

- fast interactive mode;
- settled quality mode;
- ghosting/full-refresh budget;
- image/text quality tuning.

### M5 — persistence and hardening

- tabs/history/cookies/bookmarks;
- downloads/file chooser where practical;
- pairing/authentication;
- screen-off soak;
- reconnect soak;
- memory/network failure tests.

### M6 — optional Bluetooth path

Only after Wi-Fi browser mode is stable:

- BLE control/discovery;
- measure real throughput;
- decide whether framebuffer fallback is worth implementing.

## 15. Acceptance criteria for the first usable version

A release candidate is not complete until a real M4 and real Android phone can demonstrate all of the following:

1. start Browser Bridge on Android;
2. open the Browser entry on M4;
3. load an HTTPS JavaScript-heavy site;
4. browse using M4 touch only;
5. drag-scroll a long page;
6. follow links and use back/forward/reload;
7. type a URL;
8. type ASCII and Chinese text into a web form;
9. receive local dirty-region refreshes instead of a full frame for small page changes;
10. reconnect after Wi-Fi interruption without stale framebuffer corruption;
11. keep the physical phone display off for a 30-minute browsing soak;
12. keep M4 heap/PSRAM bounded during repeated navigation;
13. preserve all existing Screen Bridge accessibility/XHS/reader modes.

Useful measurements to record:

- touch-to-first-patch latency p50/p95;
- patch-to-panel-start latency;
- bytes per tap/navigation/scroll second;
- percentage of updates that are partial vs keyframe;
- M4 internal heap and PSRAM before/after soak;
- Android memory for 1, 3 and max configured tabs;
- reconnect/resync count;
- panel cleanup/full-refresh frequency.

## 16. Deliberate non-goals for v1

- streaming video to the e-ink panel;
- 30/60 fps remote desktop behaviour;
- Chrome extensions;
- external Chrome process control;
- generic Android app mirroring through the browser protocol;
- expanding m4sim merely to simulate WebView/Android behaviour.

The repository currently treats m4sim as frozen for ordinary firmware work. Browser Bridge should rely on pure protocol/unit tests plus real Android + real M4 integration unless the implementation exposes an actual simulator correctness gap.

## 17. First implementation recommendation

Implement M0 before any large firmware patch.

The decisive technical risk is not TCP or M4 drawing; the repository already has those foundations. The decisive risk is whether an app-owned Android virtual display containing a real WebView reliably keeps rendering and producing `ImageReader` frames while the physical phone screen is off on the target phone.

If M0 passes, proceed with the architecture above. If it fails due to a device/OEM WebView lifecycle restriction, keep the protocol and M4 design but change only the Android rendering backend.

## 18. Display / presenter safety invariants (current line)

Later Browser Bridge work, including LAN discovery, must not regress these. They are already implemented; discovery is control-plane only.

- Frozen PSRAM `presentBuf`: Home must not blank an in-flight FULL. FULL uses `waveformLabBaseline(presentBuf)`; lastPresented is copied from presentBuf only on success.
- Physical `lastPresented` baseline is invalidated on disconnect, UI write, panel reinit, and failed present. Uncertain baselines stay untrusted.
- `FRAME_ACK` is independent of physical refresh. Session commits accepted CRC even if the presenter is busy or failed.
- Dense / fragmented / recover / FirstBaseline stay absolute FULL. Hygiene is stock HALF from the frozen present snapshot, driven by unique coverage / transition churn — not a fixed tiny-Partial count.
- No custom LUT / voltage / waveform invention. Hygiene uses the existing stock HALF path.
- No Accessibility / root / adb / global input as product behavior. Browser input is owned WebView injection via M4B3 TOUCH.
- Discovery must not change M4B3 framing, ACK, display, or input. Firmware advertises `_m4b3._tcp` only while the STA listener has a valid IPv4 and never calls `MDNS.end()`.

#34 optical PASS is closed at `8ff3285`. Remaining #39 gate is real-device AUTO discovery / reconnect on the integrated head. Do not claim that from host tests.