# HANDOFF — Murphy M4 session entry

Last updated: **2026-08-19**

This file is intentionally thin. **Active goals, progress, measurements and acceptance evidence live in GitHub Issues, not here.** This update exists so a new session can resume Browser Bridge work without reconstructing #34 / #39 / #42 / #44 state from chat.

## New conversation bootstrap

Use `einklover/m4-stack` as the durable source of truth. Read in order:

1. `AGENTS.md` — permanent rules / safety / architecture boundaries
2. `HANDOFF.md` — this entry point
3. `docs/FAST_FIRMWARE_DEV.md` — fast build/test/cache workflow
4. the active GitHub Issue(s) below
5. current branch HEAD / recent CI

Do not reconstruct current project state from old chat history.

## Authoritative staged heads (2026-08-19)

#34 optical PASS is closed at `8ff3285`. #39 host discovery and #34 hygiene diverged after `821acd8` and are integrated with a normal no-force merge (both ancestors required). M5 #42 productization continues from that validated Browser Bridge ancestry, and M6 #44 is now a parallel product layer based on the current M5 head.

```text
229543e  #33 white-glass / frozen presentBuf
   └── 821acd8  #34 nearby-window merge
          ├── 8ff3285  #34 content-aware HALF hygiene (optical PASS, CLOSED)
          └── bd23f73  #38 host-soak tests
                 └── f0cfeb3  #39 host-only LAN discovery
                        └── a7aee33  #39 morning handoff docs
                               └── MERGE  #39 + #34 hygiene
                                      └── ... #42 M5 productization
                                             └── f3d8809  # M5 reconnect/HELLO gate checkpoint
                                                    └── 5889062  #44 M6 first browser-shell batch
```

| Issue | Head | Branch | Status |
|-------|------|--------|--------|
| **#34** refresh hygiene | `8ff32859cb52dd82a6fdc4990337ff5988a69ee5` | `agent/eink-browser-bridge-refresh-flicker` | **CLOSED.** User optical PASS. Count-8 is not a sole FULL trigger; content-aware unique coverage / transition churn drives stock HALF hygiene. |
| **#38** host soak | `bd23f7317c132bde937abe7eb0ee2d11970d8af1` | `agent/eink-browser-bridge-host-soak` | `PASS_AUTOMATED_HOST`. Tests only vs `821acd8`. |
| **#39** discovery | `57527e8919055f6f191d4f46f8a69d34d6746204` | `agent/eink-browser-bridge-discovery` | **CLOSED.** Device AUTO / manual / service-restart / M4-reboot FirstBaseline PASS on Motorola `ZY22KN7WSK` + Murphy APP1. Merge ancestors: `8ff3285` + `a7aee33`. |
| **#42** M5 Browser Bridge productization | `f3d8809500a50c602ed207a3a2103f1891428548` | `agent/eink-browser-bridge-m5-productization` | **OPEN.** M4B3Sender initialization fix independently reviewed; reconnect state + M4B3 HELLO watchdog + CONNECTED-on-HELLO_OK compiled/CI green. Real reconnect/device acceptance still required. Do not flash M4 for the current shell work. |
| **#44** M6 Browser Shell + app-owned IME | `58890629e20cf6b49d74859837257dbb25dec8a8` | `agent/eink-browser-m6-shell-ime` | **OPEN.** First browser-shell batch is `PASS_HOST_CI`: omnibox, Back/Forward/Home/Tabs/Reload/Menu, app-owned English/symbol omnibox keyboard, root touch routing, bare `data:` lab compatibility. Exact CI run `32243403516`; device UI smoke still required. |

## Closed #39 device gate

Evidence lives on GitHub Issue #39 (`m4-discovery-device-013`). Integrated head `57527e8` was flashed APP1-only @ `0x6e0000` / OTA slot 1 and installed as the debug APK. AUTO discovers `_m4b3._tcp` with empty `m4b3_host`; manual override still wins; service restart re-discovers; M4 hard-reset reconnects on the discovered endpoint with required FirstBaseline FULL and no white glass.

## Safety invariants (do not regress)

These are closed by #33 / #34 / #38 and must stay true on every later head:

- frozen PSRAM `presentBuf` (Home must not blank an in-flight FULL);
- physical `lastPresented` baseline is invalidated on disconnect / UI write / panel reinit / failed present, and is never trusted when uncertain;
- `FRAME_ACK` is independent of physical refresh (session commits accepted CRC even if the presenter is busy/failed);
- dense / fragmented / recover / FirstBaseline / cadence FULL fallbacks remain; do not force every frame Partial;
- no waveform / LUT / voltage changes in this line of work;
- no Accessibility / root / adb / global input as **product** behavior (debug-only tools are evidence, not product paths);
- discovery remains control-plane only; M4B3 framing/ACK/display/input semantics are preserved;
- M6 browser-shell actions represented by normal touch do **not** extend M4B3;
- app-owned Browser keyboard is rendered inside the same Android `Presentation`/VirtualDisplay; do not depend on system IME appearing on the M4 display;
- `data:` Browser Bridge lab/self-test pages remain bare full-screen 480×800 so old deterministic touch/landmark coordinates are not shifted by product chrome.

## Active project tracking

Umbrella production-readiness issue:

- **#17 — Murphy M4 production readiness — fonts, streaming, device validation**

Current execution issues:

- **#18 — Real-device runtime OTF/OTC soak validation**
- **#19 — JJWXC live long-catalog progressive-stream E2E**
- **#32 — M3 Browser Bridge panel framebuffer mapping and display integration**
- **#33 — M4 Browser Bridge touch return into the hidden WebView**
- **#42 — M5 Browser Bridge productization: connection UX + hardware keys** — **OPEN**; keep open through real reconnect/lifecycle acceptance.
- **#44 — M6 Browser Shell + app-owned IME inspired by Via** — **OPEN**; current product-development priority is a complete M4-operated browser experience.
- **#34 — Browser Bridge runtime FULL-refresh flicker / content-aware hygiene** — **CLOSED** at `8ff3285` after user optical PASS.
- **#38 — Host soak for #34 merge-boundary coverage** — `PASS_AUTOMATED_HOST` at `bd23f73`.
- **#39 — Browser Bridge LAN discovery / auto-connect** — **CLOSED** at `57527e8` after Motorola + M4 AUTO discovery / reconnect PASS.

M6 durable docs:

- design: `docs/superpowers/specs/2026-08-19-m6-browser-shell-ime-design.md`
- first implementation plan: `docs/superpowers/plans/2026-08-19-m6-browser-shell-first-batch.md`
- draft PR: **#47** (`agent/eink-browser-m6-shell-ime` -> `agent/eink-browser-bridge-m5-productization`)

Completed Browser Bridge milestones:

- **#26 — M0 Android virtual-display browser capture validation** — completed with real-device 480×800, HTTPS, JavaScript, and ~90s screen-off Doze evidence.
- **#27 — M1 Browser Bridge FGS + dirty-tile patch core** — completed on `agent/eink-browser-bridge`.
- **#30 — M2 Browser Bridge M4B3 TCP keyframe/patch/ACK** — completed and hardware-validated on `agent/eink-browser-bridge-m2` (`8e15f40`).

Stale discovery duplicates (no unique task or evidence vs #39):

- **#40** and **#41** — same host-only discovery goal / `bd23f73` baseline as #39. Marked duplicate of #39. History kept.
- **#37** — earlier discovery draft from `821acd8`; no unique implementation. Noted only; do not treat it as the execution issue.

Detailed progress, commit SHAs, measurements, failures, fixes and acceptance evidence belong in those Issues. Update the relevant Issue during work instead of growing this file.

## Current working branch

Firmware / production-readiness work stays on:

```text
main
```

Historical Browser Bridge stage branches remain available:

```text
agent/eink-browser-bridge-m3
agent/eink-browser-bridge-m4
agent/eink-browser-bridge-refresh-flicker   @ 8ff3285
agent/eink-browser-bridge-host-soak         @ bd23f73
agent/eink-browser-bridge-discovery         @ 57527e8
```

Current M5 productization branch:

```text
agent/eink-browser-bridge-m5-productization @ f3d8809
```

Current M6 product branch (use this for browser shell / app-owned keyboard work):

```text
agent/eink-browser-m6-shell-ime             @ 5889062
```

M6 branch is based on the M5 productization checkpoint. Keep #42 separate/open; do not merge M6 directly to `main` while M5/M6 real-device acceptance remains incomplete.

Stale / alias remotes (do not treat as execution heads; do not delete history):

- `origin/agent/eink-browser-bridge-discovery-host` == `bd23f73` (soak alias; no unique commits)
- `origin/agent/eink-browser-bridge-stability-soak` == `821acd8` (flicker alias)
- `origin/agent/eink-browser-bridge-soak` == `9f611c7` (#35 Phase A checklist only)

Stage 13 (`agent/m4-emulator-stage13-e2e-validation`) plus the 2026-08-17 QEMU AES/GDMA, TTF advance-cache, native-provider first-window, and Reader settings IA work lives on `main`. Start firmware work from `main`. Use `agent/eink-browser-m6-shell-ime` for current Browser Shell/IME product work. Do not expand m4sim for Browser Shell convenience.

Always inspect HEAD before editing.

## Stable infrastructure checkpoint

- m4sim generic smoke: PASS
- Network Manager real-Home 3-mode E2E: PASS
- m4sim: frozen for ordinary firmware work
- daily fast CI: `.github/workflows/m4-fast.yml`
- checkpoint/full simulator gate: `.github/workflows/m4sim-smoke.yml`
- Android Browser Bridge CI: `.github/workflows/android-screen-bridge.yml`
- latest M6 first-batch CI: run `32243403516` at `5889062`, success; artifact `9361677738`, digest `sha256:d32b67dafccbd2975b6e6baf551f4645765b725fd710869816bae6868644d37b`
- Paseo remote subagent dispatcher: `.github/workflows/paseo-subagent.yml` (must live on `main`); **do not use Paseo/local AI for the current user-directed M6 work**
- Dispatcher contract: `docs/PASEO_SUBAGENT_DISPATCHER.md`

Do not expand m4sim unless a production firmware change proves a simulator fidelity gap.

## Management convention

- **Issue** = goal / status / acceptance criteria / execution evidence
- **PR + commit** = implementation
- **CI** = test evidence
- **docs** = stable how-to / architecture
- **AGENTS.md** = permanent working rules
- **HANDOFF.md** = only the pointer that tells a new session where to look

When the active Issue set, branch strategy, staged heads, or stable infrastructure entry points change, update this file. Current Browser product work is tracked in #42/#44; detailed evidence belongs there.
