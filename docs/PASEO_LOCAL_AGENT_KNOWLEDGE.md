# Paseo local-agent durable knowledge

Purpose: preserve reusable facts learned by the Mac mini / Paseo local agent so later tasks do not rediscover the same device, tooling, workflow, or debugging facts.

This is a **curated engineering runbook**, not a raw task log. GitHub Issues remain the source of truth for per-task status and acceptance evidence.

## Mandatory task behavior

Every dispatched local-agent task must do this before debugging:

1. Read `AGENTS.md`, `HANDOFF.md`, `docs/FAST_FIRMWARE_DEV.md`, and the active Issue.
2. Read this document from the task branch. If the task branch does not contain it yet, read the authoritative copy from `origin/main` with:

   ```bash
   git show origin/main:docs/PASEO_LOCAL_AGENT_KNOWLEDGE.md
   ```

3. Reuse verified paths, commands, device facts, and known solutions here instead of re-discovering them.
4. At task end, include a `knowledge_delta` section in `[PASEO_RESULT v1]`.
5. If the task discovered reusable knowledge, update this document in the task worktree and push that documentation change with the task result. Do not only mention it in chat/logs.
6. If nothing reusable was learned, report `knowledge_delta: none` and do not add noise to this file.

## What belongs here

Add concise entries for facts that will save work on a later task, such as:

- exact tool / binary / runner / cache / worktree locations;
- reliable build, flash, adb, m4adb, serial, network, or test commands;
- device identifiers and interfaces that are stable and non-secret;
- recurring failure signatures and their verified root causes;
- fixes/workarounds that were actually validated;
- ordering constraints, ownership rules, race conditions, or lifecycle traps;
- safety constraints that are easy to violate accidentally;
- how to collect high-value diagnostics without OCR or destructive operations.

Do **not** add:

- passwords, Wi-Fi credentials, API tokens, cookies, private keys, or other secrets;
- huge raw logs;
- speculative hypotheses that were not verified;
- one-off task status that belongs only in the Issue;
- copied prose that already exists in another authoritative document unless a short pointer is needed.

## Entry format

Append under the appropriate section using this compact shape:

```text
### YYYY-MM-DD — <topic> — <task_id>
Context: <repo/branch/head and device if relevant>
Observed: <failure/signature/fact>
Root cause: <verified cause, or "unknown" if still unresolved>
Reuse: <exact command/path/procedure to reuse next time>
Caution: <important boundary/safety/race, if any>
Evidence: <Issue number / commit / test name / counter; no raw secrets>
```

When a newer entry supersedes an older one, edit the old entry to point at the new rule instead of leaving two conflicting instructions.

---

## Persistent environment / tooling

### 2026-08-19 — Paseo runner layout — dispatcher baseline
Context: `einklover/m4-stack`, self-hosted Mac mini runner.
Observed: task execution is isolated from the interactive checkout.
Root cause: n/a.
Reuse: runner service `~/actions-runner-m4`; installed wrapper `~/.local/bin/paseo-agent-runner`; durable runner state `~/.paseo-agent` -> `/Volumes/z/paseo-agent`; independent runner clone `/Volumes/z/paseo-agent/repos/einklover-m4-stack`; per-task worktrees under `/Volumes/z/paseo/m4crosspoint/agent-worktrees/<task_id>`; persistent per-Issue agent state under `/Volumes/z/paseo/m4crosspoint/agent-state/`.
Caution: never modify the user's interactive `work/m4-stack` checkout from a dispatched task; no force push.
Evidence: dispatcher contract / prior Browser Bridge tasks.

### 2026-08-19 — GitHub dispatcher execution model — dispatcher baseline
Context: `.github/workflows/paseo-subagent.yml` on repository default branch.
Observed: `issue_comment` dispatch runs only from the workflow present on `main`; task comments are prompts, not shell snippets.
Root cause: GitHub Actions `issue_comment` semantics and runner hardening.
Reuse: comments start with `[PASEO_TASK v1]`; use a unique `task_id`; verify semantic nested `[PASEO_RESULT v1]` rather than treating outer process completion as success.
Caution: outer wrapper/process PASS can coexist with semantic `BLOCKED`/`PARTIAL`; always inspect the structured result body.
Evidence: `docs/PASEO_SUBAGENT_DISPATCHER.md`.

### 2026-08-19 — host C++ tests need Homebrew g++-14 — m5-key-return-build-001
Context: Mac mini host tests under `/Volumes/z/paseo/m4crosspoint/agent-worktrees/<task_id>`.
Observed: `/usr/bin/g++` is Apple clang 16.0.0. Tests that include `<vector>` or `<string>` (`test_m4b3_input`, `test_m4_panel_mapper`) fail with `__builtin_ctzg` / `__builtin_clzg` undeclared. Header-only tests without those includes (`test_m4b3_key`) compile with Apple `g++`.
Root cause: Apple libc++ uses clang-only builtins that Apple's advertised `g++` driver still cannot satisfy in this SDK pairing.
Reuse: compile host native_app tests with `/opt/homebrew/bin/g++-14` even when a file header says `g++`. Example: `g++-14 -std=c++14 -Wall -Wextra -Werror -I firmware/src firmware/tests/native_app/test_m4b3_input.cpp -o /tmp/test_m4b3_input`.
Caution: `set -e` plus `cmd && run` in zsh does not abort the script when the first command fails; check each compile exit separately.
Evidence: #42 `m5-key-return-build-001`; key test PASS with Apple `g++`; input/mapper PASS only with `g++-14`.

### 2026-08-19 — fresh worktree firmware deps via existing reconstructed tree — m5-key-return-build-001
Context: isolated task worktree with no `firmware/open-m4-sdk`.
Observed: first `pio run -e murphy_m4` fails immediately: `FileNotFoundError: open-m4-sdk/libs/hardware/BatteryMonitor`.
Root cause: reconstructed SDK / third-party libs are gitignored and not present in a new worktree.
Reuse: do not re-bootstrap if a patched copy already exists. Symlink:

```bash
ln -sfn /Volumes/z/paseo/m4crosspoint/agent-worktrees/m4-refresh-hygiene-012/firmware/open-m4-sdk firmware/open-m4-sdk
BASE=/Volumes/z/paseo/m4crosspoint/agent-worktrees/m4-reboot-white-fix-006/firmware
for p in lib/Epub lib/expat lib/miniz lib/picojpeg lib/Lua lib/EpdFont/builtinFonts; do
  ln -sfn "$BASE/$p" "firmware/$p"
done
test -f firmware/src/network/updater_fw.bin || cp -a "$BASE/src/network/updater_fw.bin" firmware/src/network/updater_fw.bin
```

Hygiene copy already has `waveformLabHygiene`. Official fallback is `bash scripts/bootstrap_deps.sh` (fetches pinned `m4-device`).
Caution: never commit `open-m4-sdk`, `builtinFonts`, `updater_fw.bin`, `.pio`, or plugin/APK binaries. Prefer symlink over copying into the worktree.
Evidence: #42 `m5-key-return-build-001`; `pio run -e murphy_m4` SUCCESS in 77.84s after the symlink.

## Murphy M4 device / flash

### 2026-08-19 — production flashing safety — Browser Bridge validated baseline
Context: Murphy M4 ESP32-S3 production firmware.
Observed: Browser Bridge device work uses the production `murphy_m4` image and APP1.
Root cause: partition layout / safety policy.
Reuse: use repository APP1 flash tooling; application offset is `0x6e0000`; hash-verify the produced image and select OTA slot 1 after flashing.
Caution: never flash APP0, bootloader, partition table, QEMU profiles, or perform a full erase/factory reset unless the user explicitly authorizes it.
Evidence: `AGENTS.md`, Browser Bridge #30/#33/#34/#39 validation.

### 2026-08-19 — m4adb ownership — stable infrastructure
Context: physical M4 automation/debugging.
Observed: multiple m4adb/serial daemons can collide and produce misleading device behavior.
Root cause: competing ownership of the same device/control channel.
Reuse: use repository m4adb tooling and keep one global daemon.
Caution: never `pkill -f m4adb.py`; stop/restart through the repository-supported mechanism and record which process owns the device before debugging transport/display failures.
Evidence: `AGENTS.md` and prior Browser Bridge white-panel diagnostics.

## Browser Bridge

### 2026-08-19 — physical panel baseline is not framebuffer acceptance — #33/#34
Context: M4B3 Browser Bridge panel presenter.
Observed: Android can send a non-white frame and M4 can accept the logical CRC while the glass remains stale/white.
Root cause: physical panel baseline/lifecycle can become untrusted independently from the accepted logical framebuffer.
Reuse: when the panel may have been changed/reset outside Browser Bridge, invalidate the last physical Browser baseline; next Browser presentation must use the production recovery FULL path, and only a successful physical present may restore trust.
Caution: `FRAME_ACK` means logical framebuffer/protocol acceptance only; never couple it to physical E-ink completion. Do not infer optical success from CRC/counters alone.
Evidence: #33 white-glass diagnosis and #34 refresh-hygiene validation.

### 2026-08-19 — Browser Bridge touch ownership — #33
Context: M4 physical touch -> Android hidden WebView.
Observed: FT6x36 touch must be routed exclusively to Browser Bridge while the M4B3 session is hello-ok, otherwise local Reader/Home gestures can also consume it.
Root cause: shared physical input source.
Reuse: `MappedInputManager::setTouchRoutedToBrowser(true)` suppresses the ordinary local touch path while `M4B3Tcp::captureFromGpio()` sends the pointer over M4B3. Panel-native to logical mapping is `logicalX = 479 - physicalY`, `logicalY = physicalX`.
Caution: Android dispatch stays inside the app-owned hidden WebView; no Accessibility/root/adb/global injection as product behavior.
Evidence: #33 M4 touch-return implementation.

### 2026-08-19 — discovery precedence — #39
Context: Android Browser Bridge LAN auto-connect.
Observed: validated selection precedence is manual override > live discovered `_m4b3._tcp` > cached last-known endpoint > none; explicit `loopback`/`local` bypasses discovery.
Root cause: deterministic control-plane design.
Reuse: keep `_m4b3._tcp` mDNS/DNS-SD as control-plane only; M4B3 framing, ACK, panel and input semantics are independent. Empty `m4b3_host` is normal AUTO mode.
Caution: avoid listener/retry storms; discovery restarts are bounded. Manual host remains the deterministic escape hatch.
Evidence: #39 device AUTO/manual/service-restart/M4-reboot PASS.

### 2026-08-19 — apply_m5_key_return.py exact file set — m5-key-return-build-001
Context: `agent/eink-browser-bridge-m5-productization` at `05206c9`.
Observed: `python3 scripts/browser_bridge/apply_m5_key_return.py` applied once with every exact match found; no BLOCKED replacements.
Root cause: n/a.
Reuse: intended touched set is exactly these 12 files: `firmware/src/util/M4B3Protocol.h`, `firmware/src/MappedInputManager.h`, `firmware/src/MappedInputManager.cpp`, `firmware/src/network/M4B3TcpReceiver.cpp`, `firmware/src/network/M4B3TcpReceiver.h`, `firmware/src/main.cpp`, `android/m4-screen-bridge/app/src/main/java/com/murphy/m4screenbridge/browser/stream/M4B3.java`, `M4B3Message.java`, `M4B3Codec.java`, `browser/BrowserPresentation.java`, `browser/VirtualBrowserSession.java`, `android/m4-screen-bridge/build.sh`. Helpers `firmware/src/util/M4B3Key.h`, `firmware/tests/native_app/test_m4b3_key.cpp`, `M4B3KeyState.java`, `M4B3KeyTest.java` were already authored on the branch and are not rewritten by the script.
Caution: if any `replace_once` reports `expected one match, found N`, stop. Do not invent a replacement.
Evidence: #42 `m5-key-return-build-001`; script stdout ended `M5 Browser Bridge key-return integration applied exactly.`

### 2026-08-19 — applied key TX loop is currently unreachable — m5-key-return-build-001
Context: `firmware/src/network/M4B3TcpReceiver.cpp` after `apply_m5_key_return.py`.
Observed: `flushInput()` still `return`s when the touch queue is empty, then the authored key-drain loop is textually after that return. `captureFromGpio()` can also `return` before the appended Back/Confirm enqueue if `haveLastPanel` is false and there is no touch edge this frame.
Root cause: exact splice kept the original touch-only early exits.
Reuse: superseded by `fix_m5_key_control_flow.py` on `m5-key-return-device-005`. Do not re-diagnose this as still open after that script has been applied.
Caution: host key/input tests and `pio run` cannot see this; they will PASS with dead key TX. Do not treat compile PASS as proof that physical Back/Confirm will leave the M4.
Evidence: #42 `m5-key-return-build-001` post-apply inspection of `flushInput()` / `captureFromGpio()`.

### 2026-08-19 — dispatcher unknown workspace_id header — m5-key-return-device-005
Context: GitHub `issue_comment` `[PASEO_TASK v1]` dispatch on `einklover/m4-stack`.
Observed: `m5-key-return-device-002` RECEIVE `provider not allowed: grok` with `workspace_id` appended; `device-003`/`004` RECEIVE `invalid timeout: '120m\nworkspace_id: wks_c7bfd8e08671b105'`. `device-005` with no `workspace_id` header reached RUNNING.
Root cause: dispatcher header parser treats an unknown key as a continuation of the previous header value (`provider`/`timeout`), not as a separate field.
Reuse: do not put `workspace_id:` in the task header block. Issue-workspace reuse is automatic via the installed issue-workspace logic. Keep the header set to documented keys only (`task_id`, `issue`, `repo`, `branch`, `expected_head`, `mode`, `timeout`, optional `provider`).
Caution: a GitHub Actions outer FAIL can be RECEIVE-only; no worktree/code work happened. Inspect the structured `[PASEO_RESULT v1]` `failure_stage` before retrying as if the agent ran.
Evidence: #42 comments 5336308579 / 5336360282 / 5336384209 vs RUNNING 5336441906.

### 2026-08-19 — M5 key control-flow script — m5-key-return-device-005
Context: `agent/eink-browser-bridge-m5-productization` at `ea3c4bb`.
Observed: `python3 scripts/browser_bridge/fix_m5_key_control_flow.py` printed `applied M5 key control-flow correction: 2 exact replacements`. Diff was only `firmware/src/network/M4B3TcpReceiver.cpp`.
Root cause: n/a.
Reuse: run exactly once from the isolated task worktree. Intended production change is: touch-queue empty `return` -> `break` in `flushInput()`, and Browser Back/Confirm enqueue moved before the touch-only early return in `captureFromGpio()`.
Caution: if the script does not report exactly two replacements, STOP. Do not invent a replacement.
Evidence: #42 `m5-key-return-device-005`.

### 2026-08-19 — debug APK signature mismatch — m5-key-return-device-005
Context: Motorola `ZY22KN7WSK` already had `com.murphy.m4screenbridge` from a prior worktree debug keystore.
Observed: `adb install -r` failed `INSTALL_FAILED_UPDATE_INCOMPATIBLE` (signatures do not match).
Root cause: isolated worktrees sign with different local debug keystores.
Reuse: `adb -s ZY22KN7WSK uninstall com.murphy.m4screenbridge` then `adb install` the just-built `android/m4-screen-bridge/m4-screen-bridge-debug.apk`. This is not a factory reset.
Caution: uninstall clears that app's prefs (`m4b3_host` becomes empty AUTO). Do not `pm clear` unrelated packages or wipe device data.
Evidence: #42 `m5-key-return-device-005`; lastUpdateTime 2026-08-19 09:43:17 after reinstall.

### 2026-08-19 — empty am extra swallows next flag as m4_host — m5-key-return-device-005
Context: launching Browser Bridge with `am start ... --es m4_host "" --ei m4_port 48624`.
Observed: session became `M4B3 manual --ei:48624` / `UnknownHostException: --ei`.
Root cause: empty `--es` value is not a safe empty string; `am` treats the next token as the extra value.
Reuse: omit host extras for AUTO (fresh install default is empty host). For manual: `--es m4_host 192.168.0.152 --ei m4_port 48624`. Diagnose with `dumpsys activity service ...BrowserBridgeService`.
Caution: a bad extra also writes SharedPreferences via `handleLabIntent`/`saveM4Host`; STOP then relaunch with a real host to overwrite.
Evidence: #42 `m5-key-return-device-005` first INPUT_TEST launch.

### 2026-08-19 — m4b3_status empty object vs panel — m5-key-return-device-005
Context: post-flash M4 with hello-ok Browser session.
Observed: `m4adb m4b3_status` printed `{}` while `m4b3_panel` and Android dumpsys showed accepted CRC `0xE1009E63` / presenter `full_ok=2`.
Root cause: likely client JSON parse of a truncated `m4b3_status` snprintf (`out[1728]` in `M4SerialDebugBridge.cpp`) rather than a dead receiver. Key counters are not in that JSON yet; they live in Serial `logSnapshot` `key(back=... txerr=...)`.
Reuse: treat `m4b3_panel` + Android `dumpsys activity service com.murphy.m4screenbridge/.browser.BrowserBridgeService` as the reliable hello-ok/ACK/presenter/key-dispatch evidence. Do not start `m4adb logs` in parallel with `status` (`listen(1)`).
Caution: a leftover `m4adb logs` client plus a second `daemon --socket /tmp/m4adb-*.sock` can both hold `/dev/cu.usbmodem101`. Kill only the extra logs/daemon PIDs; keep the earliest healthy daemon.
Evidence: #42 `m5-key-return-device-005`; panel accepted_crc 3774914147 == Android crc 0xE1009E63.

### 2026-08-19 — flash_app1_once cwd and wifi_prepare — m5-key-return-device-005
Context: APP1 flash from an isolated task worktree.
Observed: `firmware/scripts/flash_app1_once.sh` writes APP1, hash-verifies, switches OTA slot 1, and restarts one daemon. Device comes up `wifi_connected=false`.
Root cause: helper default firmware path is `.pio/build/murphy_m4/firmware.bin` relative to cwd; saved STA is not auto-joined after reboot.
Reuse: `cd firmware && bash scripts/flash_app1_once.sh /dev/cu.usbmodem101`. Then `m4adb wifi_prepare` before Browser TCP. Confirm hash `Hash of data verified` and `OTA slot 1 selected`.
Caution: flash script stops all m4adb first (required). After reboot, first `status` may race; retry, do not re-flash.
Evidence: #42 `m5-key-return-device-005`; firmware sha256 `30c404b043f4f51341f649d45cf94520d070b73f3f7f73072ff80a241d84f71b`; STA `192.168.0.152`.

### 2026-08-19 — wiki DenseArea used to force OTP FULL ~4s — m5-touch-realweb-008
Context: `agent/eink-browser-bridge-m5-productization` at `b5825cf` on Motorola `ZY22KN7WSK` + M4 STA `192.168.0.152:48624`, URL `https://zh.m.wikipedia.org/wiki/电子纸`.
Observed: every large page load/scroll felt like a 4s full refresh.
Root cause: dirty planner treated `changedPixels > 28%` (`kMaxPartialChangedPixels=107520`) as `Mode::Full`/`DenseArea`, and `M4B3Panel::tick` drove Full/Hygiene through `waveformLabBaseline` (SSD1677 OTP FULL ~4s) / `waveformLabHygiene` (stock HALF). Extra Dim wiki frames routinely exceed 28%.
Reuse: superseded by FAST-only present policy below. Diagnose 4s stalls with `m4b3_panel` `full_ms` (~4000 = OTP FULL, hundreds = FAST) plus Serial `present-start`; do not use OCR.
Caution: `full_ok` / `Mode::Full` are policy labels, not physical waveform proof.
Evidence: #42 user report + pre-change `M4B3Panel.cpp` Full branch.

### 2026-08-19 — Browser Bridge presents are FAST-only — m5-touch-realweb-008
Context: same branch after user request to forbid full refresh.
Observed: post-flash hello-ok on the same wiki session: first present `full_ms=630`, later fragmented Full still `full_ms=631`, Partial `part_ms=517..2031`, `hyg_ok=0`. No ~4000ms OTP FULL.
Root cause: presenter now always uses stock `displayWindow` FAST/DU. Dense with 1–4 windows stays Partial; first/recover/fragmented/hygiene still request a full-panel update but physically `displayWindow(0,0,800,480)`.
Reuse: do not restore `waveformLabBaseline` / `waveformLabHygiene` on the Browser Bridge path. Host check: `g++-14 -std=c++14 -Wall -Wextra -Werror -I firmware/src firmware/tests/native_app/test_m4_panel_dirty.cpp` (dense 400×300 expects Partial+DenseArea). Device check: `m4adb wifi_prepare` then `m4b3_panel`; existing Android FGS with saved MANUAL host reconnects without a new `am start`.
Caution: ghosting will increase; `kMinIntervalMs` is still 2000. `full_ms≈600` is a full-panel FAST, not OTP. Firmware sha256 `1c3a17cb37bbcf28807c188c4c72f5f844a34356af266abd8f44fcbd1ed8e192` flashed APP1 @ `0x6e0000` / OTA slot 1.
Evidence: #42 `m5-touch-realweb-008`; `test_m4_panel_dirty` / `test_m4_panel_presenter` PASS; panel samples in `build/m5-touch-realweb-008/panel-*.json`.

### 2026-08-19 — BrowserBridgeService is not exported — m5-product-session-010
Context: `agent/eink-browser-bridge-m5-productization` on Motorola `ZY22KN7WSK`.
Observed: `adb start-foreground-service ...BrowserBridgeService` is rejected; service `android:exported="false"`.
Root cause: product FGS is app-private; lab/control intents are on exported `MainActivity`.
Reuse: drive start/stop/lab pages with `am start -n com.murphy.m4screenbridge/.MainActivity -a com.murphy.m4screenbridge.browser.{STOP,INPUT_TEST,SELF_TEST,LANDMARK}`. Product URL start is the on-screen `启动/切换网页（自动恢复）` button (or `BrowserBridgeService.startUrl` in-process). Diagnose with `dumpsys activity service com.murphy.m4screenbridge/.browser.BrowserBridgeService`.
Caution: `onCreate` also calls `resumeIfConfigured` unless the launch action is a lab control intent. Do not treat a missing dumpsys service as a crash if STOP just ran.
Evidence: #42 `m5-product-session-010`; STOP via MainActivity delivered to running instance and `stopSessionAndSelf` ran.

### 2026-08-19 — am extra spaces split the host token — m5-product-session-010
Context: invalid-manual-host fail-soft (`Prefs.m4b3HostRaw` / `M4LanDiscovery.validHost`).
Observed: `--es m4_host 'not a valid host!!!'` became `mode=manual endpoint=not:48624` reconnect storm. `--es m4_host 'badhost!!!'` fail-softed to AUTO `192.168.0.152:48624`.
Root cause: `am` splits the extra on spaces; only the first token is stored. `not` classifies as a MANUAL host (`validHost` true enough to not fail-soft).
Reuse: never put spaces in `--es m4_host`. Use a punctuation-only invalid token such as `badhost!!!` to exercise fail-soft. Empty host must omit the extra or use UI clear+`保存 M4 地址` (empty `--es` still swallows the next flag; see prior entry).
Caution: a bad extra writes SharedPreferences via `handleLabIntent`/`saveM4Host`. Runtime AUTO from fail-soft does not by itself clear the stored raw string.
Evidence: #42 `m5-product-session-010` dumpsys_J.txt vs dumpsys_J2.txt.

### 2026-08-19 — Motorola DeviceGuard AutoRun kills BOOT_COMPLETED restore — m5-product-session-010
Context: XT2437-4 / `paros_cn` serial `ZY22KN7WSK`; app already on `dumpsys deviceidle whitelist`.
Observed: after `adb reboot`, `sys.boot_completed=1` at 2026-08-19T05:33:44Z. Logcat: `Start proc 7875:com.murphy.m4screenbridge ... for broadcast {...BrowserBridgeStartupReceiver}` at 13:34:02.251, then 71ms later `Force stopping com.murphy.m4screenbridge ... from pid 5015 and uid 10267 (com.motorola.deviceguard)` / DeviceGuard `[AutoRunServices]`. Shell cannot send `BOOT_COMPLETED` (`SecurityException`).
Root cause: OEM AutoRun killer, not missing `RECEIVE_BOOT_COMPLETED` / receiver registration. Product receiver did run.
Reuse: Gate G must capture ActivityManager start-proc + DeviceGuard force-stop lines. Prefs still survive: later `am start -n com.murphy.m4screenbridge/.MainActivity` restored `lastUrl` + AUTO. Do not treat DeviceGuard kill as an M4 firmware issue.
Caution: a second phone reboot will not prove `resumeEnabled=false` vs AutoRun; use explicit STOP + cold launch for Stop semantics. Do not disable DeviceGuard unless the user asks.
Evidence: #42 `m5-product-session-010` `/tmp/m5-product-session-010/G_logcat_boot.txt`.

### 2026-08-19 — M4B3 can stay CONNECTED with CRC 0 until a new process + live listener — m5-product-session-010
Context: Android-only M5 session against existing FAST firmware; no M4 flash.
Observed: `svc wifi disable` incremented `reconnects` while dumpsys stayed `state connected`. After Wi-Fi restore and after a non-flash M4 DTR reboot, dumpsys stayed CONNECTED with `crc 0x00000000` / `patch frame -1` / `pending`. Host `TCP 192.168.0.152:48624` still opened. A later `am force-stop` + MainActivity launch (new pid) completed HELLO (`crc 0xE415B746`, panel `owner=2` `full_ms=630`).
Root cause: transport/session can keep a zombie CONNECTED view when the same process reuses a sender/socket against a wedged or half-open M4 listener; HELLO does not complete until a new Android process talks to a live listener. `startTcpTransport` must construct `sender = new M4B3Sender(next)` before `next.start()` (mechanical fix `3a8a757`).
Reuse: do not accept `state connected` without a non-zero CRC and advancing `patch frame`/`m4b3_panel` `age_ms`. Optical overlay is not machine-readable here; use dumpsys/logcat. Distinguish synthetic dumpsys taps from physical M4 touch.
Caution: airplane-mode is not a reliable TCP interrupt on this Motorola (false idle). Prefer `svc wifi disable` or M4 DTR reboot. Never flash M4 for an Android-only recovery task.
Evidence: #42 `m5-product-session-010`; dumpsys_D3_poll.txt CRC 0 vs dumpsys_F.txt CRC `0xE415B746`.

### 2026-08-19 — M4 DTR reboot without firmware flash — m5-product-session-010
Context: transport-interrupt recovery when `svc wifi` leaves a zombie TCP.
Observed: stopping the unique m4adb daemon, toggling DTR/RTS on `/dev/cu.usbmodem101`, then starting one new daemon reset M4 panel counters (`epoch` increment, `trusted` false until a new HELLO) without APP1 write.
Root cause: USB serial DTR/RTS resets the ESP32; this is not an OTA/APP1 flash.
Reuse: only when the current daemon is dead or a reset is required: stop m4adb PIDs via `ps -axo pid=,command= | awk '/[m]4adb\.py/ {print $1}'` (never `pkill -f m4adb.py`), `rm -f /tmp/m4adb-*.sock`, DTR pulse, then `nohup ... m4adb.py daemon --ready-timeout 90`. Keep the new unique daemon; do not restart a healthy one.
Caution: Android FGS can survive the M4 reboot and remain zombie CONNECTED; a new Android process is still required for HELLO. Firmware flash remains forbidden for Android-only tasks.
Evidence: #42 `m5-product-session-010` DTR path; daemon pid 27186 after reboot.

### 2026-08-19 — dumpsys snapshot can hide MainActivity controls — m5-product-session-010
Context: uiautomator on `MainActivity` while INPUT_TEST `data:` URL is live.
Observed: `browserStatusView` snapshot includes the full `data:` URL, so the status TextView fills the screen (`nodes=6`) and URL/host/start buttons are far below. One BACK from an EditText can leave the app to the launcher before Save is tapped.
Root cause: snapshot string is unbounded.
Reuse: swipe 5–8 times `input swipe 540 1700 540 700` until `EditText`/`保存 M4 地址` appear. Hide IME by switching to `com.android.inputmethod.latin/.LatinIME` and deleting; do not KEYCODE_BACK until Save/Start is tapped. `run-as` is not available on this installed APK (`package not debuggable` even for the debug-signed artifact).
Caution: Sogou IME concatenates typed URLs. Prefer LatinIME + DEL-clear + `input text`.
Evidence: #42 `m5-product-session-010` uidump_preG.xml vs uidump_cleared2.xml.
