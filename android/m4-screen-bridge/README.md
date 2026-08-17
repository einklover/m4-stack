# M4 Screen Bridge (Android)

Android accessibility bridge to Murphy M4. It serves both the original 1bpp
screen stream and a structured content API over fixed TCP port **48624**. The
structured API currently exposes the phone launcher directory plus Xiaohongshu
image/text recommendations, readable note text and comments. Video and live
cards are rejected rather than opened as a fallback.

Zero third-party dependencies (no AndroidX). minSdk 30 (AccessibilityService
`takeScreenshot` requires API 30).

## Wire protocol (server on the phone)

All multi-byte header fields are little-endian.

- `GET /v1/status` -> JSON: `{"ok":true,"port":48624,"active":true,"cacheEnabled":true,"captureErrorCode":0,"pages":{"lo":..,"hi":..,"count":..},"consumed":N,"connected":true|false}`
- `GET /v1/page?index=N` -> `200` with a 24-byte header followed by payload,
  or `404` JSON when page N is not (yet) available.

  24-byte header fields:
  | field | type |
  |---|---|
  | magic `"M4R1"` | 4 bytes |
  | version = 1 | u8 |
  | codec (0 raw1, 1 custom RLE1) | u8 |
  | width = 800 | u16 LE |
  | height = 480 | u16 LE |
  | stride = 100 | u16 LE |
  | page | i32 LE |
  | rawSize = 48000 | u32 LE |
  | crc32 of decoded physical framebuffer | u32 LE |

- `POST /v1/consume?index=N` -> `200` JSON, returns promptly. Tells the phone
  the M4 finished page N; prefetch continues on its own thread.
- `POST /v1/tap?x=X&y=Y` -> `200` JSON. In realtime mode, maps the M4 logical
  480x800 coordinate onto the letterboxed phone screen, taps it, then captures
  a fresh page 0.

Structured content endpoints used by the native XML plugin:

- `GET /v2/apps`, `POST /v2/apps/open?id=PACKAGE`
- `GET /v2/xhs/feed`, `POST /v2/xhs/feed/open?token=TOKEN`
- `GET /v2/xhs/note`
- `GET /v2/xhs/image?index=N` (480x650 1bpp BMP)
- `POST /v2/xhs/comments/open`, `GET /v2/xhs/comments?advance=0|1`

The XHS adapter is autonomous: it recognizes the foreground Activity and
semantic view anchors, navigates back to Discover/Recommend, scrolls the feed,
and opens visible image/text cards before RecyclerView recycles them. It caches
the note body, comments, and e-ink-ready images, then returns to the feed. Only
ready entries are exposed to M4. Stable feed tokens select cached records, so
reading no longer depends on the phone showing the same page.

Framebuffer: physical 800x480, MSB first, 1 = white, 0 = black. Logical
portrait (x 0..479, y 0..799) maps to physical `phyX = y`, `phyY = 479 - x`,
byte `phyY*100 + phyX/8`, bit `7 - (phyX%8)`. RLE1 tokens: `1` high bit = run
length `low7+3` + one byte; `0` high bit = literal `low7+1` bytes. RLE is used
only when the payload is smaller than raw 48000 bytes.

## What it does

1. The accessibility service starts the fixed-port server but does not touch the screen until the user taps Start session.
2. After a five-second switch-back delay, captures page 0 and prefetches pages 1 and 2 by tapping Fanqie's right-hand page zone.
3. Each consume moves the prefetch window ahead by up to 2 pages.
4. Keeps at least 8 pages behind the consumed index for going back.
5. Pipeline: crop/scale to logical 480x800 (fit or cover) -> grayscale ->
   horizontal text-band detection with blank gaps capped at `max_gap` ->
   threshold or Floyd-Steinberg dither -> direct physical framebuffer packing.
6. With caching disabled, keeps only realtime page 0 and forwards M4 touches to
   the corresponding phone position.
7. If cached mode detects an ad, popup, or other non-reading screen, it switches
   to realtime mode automatically. It periodically rechecks the screen and
   resumes cached auto-layout/prefetch when body text returns.

## Setup

1. Build: `./build.sh` (local Android SDK tools only; no Gradle/network).
   Off-device tests: `./build.sh --test`.
   Output: `m4-screen-bridge-debug.apk` (debug-signed, installable).
2. `adb install -r m4-screen-bridge-debug.apk`.
3. Open the app, enable the accessibility service.
4. Return to the bridge app, tap Start session, then switch to the desired book
   page during the five-second delay.
5. Keep the screen on/unlocked and the book app in the foreground. Tap Stop
   session before leaving the reader.
6. The M4 discovers the phone from its saved Wi-Fi-transfer visitor IPs and
   connects to TCP port 48624.

## Tunables (SharedPreferences, edited in the UI)

| key | default | meaning |
|---|---|---|
| `threshold` | 128 | grayscale cutoff; lower if noisy, raise if text vanishes |
| `max_gap` | 12 | max blank rows kept between detected text bands |
| `prefetch_ms` | 900 | delay between prefetch swipe and next capture |
| `dither` | false | Floyd-Steinberg dithering instead of hard threshold |
| `crop_mode` | `fit` | `fit` = whole screen contained; `cover` = fills panel |
| `cache_enabled` | true | prefetch/history; false = realtime page 0 + touch forwarding |

## Layout

```
app/src/main/
  AndroidManifest.xml
  java/com/murphy/m4screenbridge/
    MainActivity.java       simple UI: accessibility shortcut, start/stop, tunables
    ScreenBridgeService.java accessibility service: capture, swipe prefetch, pipeline
    Framebuffer.java        physical packing (pure Java)
    Preprocess.java         crop/scale/gray/threshold/dither/band-gap (pure Java)
    Rle.java                RLE1 codec (pure Java)
    Header.java             24-byte LE header (pure Java)
    PageStore.java          bounded page cache (pure Java)
    HttpServer.java         minimal HTTP server, port 48624 (pure Java)
    BridgeContentApi.java   launcher and structured Xiaohongshu accessibility API
    XhsFeedParser.java      fail-closed image/text card classifier
    Prefs.java              tunable clamping
  res/...                   manifest, strings, accessibility config, launcher
app/src/test/java/.../TestMain.java   off-device self-checks (RLE, header, packing, gaps)
build.sh                    SDK-tools build + self-check
build.gradle                equivalent Gradle build (needs AGP, optional)
```

## Known limitations

- `takeScreenshot` fails when the screen is off or locked; keep it on.
- Page 0 is whatever is on screen after the five-second start delay; the book
  app must be foreground.
- Cached automatic page turn taps the right-hand page zone for Fanqie-style
  paged readers. Realtime mode forwards the actual M4 touch instead.
- If a tap fails to turn the page, the duplicate screen is still stored as
  the next page index (kept simple; M4 will just show the same page).
- Xiaohongshu view ids and descriptions are not a public API. The adapter uses
  multiple fallbacks, but a future app release may require selector recovery.
- Structured XHS reading requires the phone unlocked and its accessibility
  service enabled. The app may visibly navigate while it fills the cache.
