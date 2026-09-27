# M4 zxl handoff — 2026-09-27

This session is the grok-low / zxl4869 Grok 4.7 close-out of the dirty tree on
`feature/m4-stability-astra-20260926`. Parent is
`2e75e2b140a642a4095f494ae96cb7313f1b6e9d` (already on origin). Astra was not
called. songzhangchi01 was not called. The local `pio` processes song left
behind had already exited; their logs were reused and `pio` was not run again.

Worktree: `/private/tmp/m4-stability-astra-20260926`. No Paseo edits, no
real-device flash, no user SD. Firmware `.bin` files stay untracked build
products. `docs/M4_ASTRA_STABILITY_AUDIT.md` was not rewritten. The audit is
not finished.

Integration commit: `e0c9388cf6f79831cc3379eb6769bd2a9e9942cc`

## What "closed" means here

A batch is closed only where this turn has a command result. APP1 and the
QEMU plugin image both compiled the whole dirty tree. The QEMU run is a boot
ping on a fresh FAT image. It does not open the drawer, the installer, or
ScreenBridge, and it is not a device run.

## Builds reused

Both PIDs (29498, 41132) were already gone. No PlatformIO process was running.
No `cpp` / `h` / `py` under `firmware/`, `docs/`, or `simulator/` was newer
than the QEMU plugin image, so the images match this source.

| Env | Log | Result | RAM | Flash (limit 7,143,424) | Local image |
| --- | --- | --- | --- | --- | --- |
| `murphy_m4` | `/tmp/m4-song-pio-app1.log` | SUCCESS 30.81 s, `APP1_RC:0` | 108,908 / 327,680 (33.2%) | 5,776,925 (80.9%) | `firmware/.pio/build/murphy_m4/firmware.bin` |
| `murphy_m4_qemu_plugin` | `/tmp/m4-song-pio-qemu.log` | SUCCESS 29.56 s, `QEMU_BUILD_RC:0` | 108,204 / 327,680 (33.0%) | 5,786,549 (81.0%) | `firmware/.pio/build/murphy_m4_qemu_plugin/firmware.bin` |

SHA-256 of those local images, not committed and not flashed:

- APP1 `f30a76267aadec6c3552c12689b03ae4f98c9b28396ffd6df24c21cb0b9b5316`
- QEMU plugin `0902839d257994cbe8cdf6a9f467f9302e8f5c9b4fbccfe5680073616a42bf61`

The previously published APP1 digest predates this tree. These two digests
replace it only as local build identity.

## Isolated QEMU smoke

`127.0.0.1:18080` is owned by mihomo, and `./m4sim test smoke` always adds
`hostfwd` for that port. This run called the same `cmd_run` path with
`--skip-build`, `fresh_sd`, and `no_hostfwd`. Session root
`/tmp/m4-zxl-smoke-2e75e2b` (also `M4SIM_TMP`). The SD image is the regular
file `/tmp/m4-zxl-smoke-2e75e2b/artifacts/murphy-sd.img`, 64 MiB FAT32, created
by `mformat` (`simulator/qemu/make_sd_image.py`). QEMU was
`-drive file=<that image>,if=sd,format=raw` plus the composed 16 MiB flash
`14d1f26e712f09d1357851fca3017cc0ba1ddbe4ffded3fef1f800978a263b7f`. Serial was
PTY `/dev/ttys235`. Every `m4adb` call was `--port /dev/ttys235 --no-daemon`.
No USB modem node was present. The session was stopped afterwards; QEMU pid
55699 is gone and `state.json` was removed.

The first ping attempt timed out inside the ready wait. The second attempt
was ready in 0.5 s. The explicit ping after ready returned `PING_RC 0` and
included `"protocol": 1`. Status after that:

- `firmware`: `202608187-murphy-m4-qemu-plugin`
- `activity`: `Home`
- `sd_ok`: true
- `free_heap`: 162368, `free_psram`: 1769116
- `wifi_ssid`: `qemu-openeth`, `wifi_ip`: `10.0.2.15`
- frame `/tmp/m4-zxl-smoke-2e75e2b/artifacts/smoke.pbm`, 48011 bytes

Printed `SMOKE PASS`. That is boot readiness on an empty card image. It is
not an AppList, install, or long-run heap measurement.

## Host regressions this turn

All commands were from the worktree. Logs are under `/tmp/m4-zxl-host-regress/`.
Every log ends `EXIT:0`. The script printed `HOST_REGRESS_OK`.

| Command | Result line |
| --- | --- |
| `python3 firmware/tests/test_m4_app_list_lifecycle.py` | `pending-handoff: old_items=4 old_apps=2 accepted_items=7 selected=6 copy_delta=2 remember=1 ensures=1` then `actual app list lifecycle: PASS` (printed twice) |
| `c++` + `/tmp/m4-zxl-drawer-handoff` | `app drawer handoff contracts: ALL PASS` |
| `c++ -I firmware/src` + `/tmp/m4-zxl-plugin-paint` | `PLUGIN_ENTRY_PAINT_CONTRACT_OK` |
| `python3 firmware/tests/test_m4_screen_bridge_lifecycle.py` | `actual screen bridge lifecycle / cancel, publish, native app ready: PASS` |
| `python3 firmware/tests/test_m4_batch_install_scan.py` | `exit-wait: elapsed_ms=57` and `actual batch install scan: PASS` |
| `python3 firmware/tests/test_m4_astra_stability.py` | directory move guard PASS, 4000 early returns PASS, directory scanner PASS |
| `python3 firmware/tests/test_m4_http_body_transaction.py` | `actual HTTP sink+download transaction / bounded chunks and 8 failure modes: PASS` |
| `python3 firmware/tests/test_m4_storage_failures.py` | Clear Cache PASS, TXT progress transaction PASS |
| `python3 firmware/tests/test_m4_sd_retry_contract.py` | SD retry input PASS, capability probe PASS |
| `python3 firmware/tests/test_m4_install_completion.py` | `actual installer worker / UI reclaims job immediately at done publication: PASS` |
| `python3 firmware/tests/test_m4_font_loader_contract.py` | `m4 font loader contract passed` |
| `test_m4_download_commit.cpp`, `test_m4_cache_clear_policy.cpp` | compile and run `EXIT:0` (assert-only, no extra print) |

Lifecycle also kept the earlier retirement numbers on this same run:
`display-hold` `locks=2` `on_exit_ms=0`, `pending-discard` `remember=0`,
`dirty-frame: copy_assigns=2 copy_ctors=0`.

## Per-batch closure

| Batch | Source | Host | In the two SUCCESS images | QEMU runtime | Still open |
| --- | --- | --- | --- | --- | --- |
| Library page cursor and directory-into-itself cluster guard | `MyLibraryActivity.cpp` | astra stability PASS | yes | not driven | device directory of thousands of books |
| ScreenBridge stop, last-slice cancel, deferred Home HTTP shutdown | controller, `NativeAppActivity`, `main.cpp` boundary | screen lifecycle PASS | yes | Home only; bridge not opened | device / a real HTTP provider |
| BatchInstall single open, bounded names, UI snapshot | `BatchInstallActivity` | scan PASS | yes | not driven | `onExit` still waits (`elapsed_ms=57`). No process-wide install mutex |
| AppList pending drawer | `pendingApps_` / `pendingItems_` / `pendingReady_`. Worker does not assign `apps_`, `items_`, `selectedIndex_`, or `mode_`. `loop` accepts before `submitDirtyFrame`. Exit discards before the release store | lifecycle + handoff + paint PASS | yes | drawer not opened | unlocked UI reads stay; the worker no longer writes those fields |
| HTTP body bound + `M4DownloadCommit` | `HttpDownloader.cpp`, new header | http + download commit PASS | yes | not driven | real TLS / chunked parser is outside this host shim |
| TXT `progress.dat` commit and Clear Cache busy/name checks | reader, settings, `M4CacheClearPolicy.h` | storage PASS | yes | not driven | a live reader on device |
| `SDCardManager::begin` returns if already mounted; `capabilityProbe` fails on root `getError` | SDK cpp/h | sd retry PASS | yes | empty image `sd_ok=true` only | not a fault injection on QEMU |
| `removeDirAtDepth` shares one 768-byte name and fails the directory on `getError` | SDK cpp/h | no dedicated scenario | yes | not driven | host coverage of this hunk is missing |
| App Store rejects a download unless `sync` and `close` succeed | `AppStoreActivity.cpp` | no extracted body | yes | not driven | host coverage of this hunk is missing |
| Install worker publishes `done` after its last use of `InstallJob` | 3-line diff | trampoline PASS | yes | not driven | the 60 s UI wait below |

`docs/M4_ASTRA_GROK_FOLLOWUP.md` is the earlier stage log. Its "no commit"
lines describe those passes. They are not edited to pretend the commit
already existed.

## Not closed

### AppInstall 60 s ownership

`AppInstallActivity::doInstall` (lines 113–162) still creates `InstallJob` on
the heap, then the UI task spins up to 60 s. On timeout it stores
`installRunning_ = false` and returns without deleting the job, while
`installTaskTrampoline` keeps writing `job->result`. `onExit` (lines 183–197)
waits at most 2 s, then deletes the display task and the render mutex.
`test_m4_install_completion.py` only checks that the trampoline release-stores
`done` and then self-deletes. It does not cover the timeout or `onExit`.

Required shape, not implemented in this commit: the activity owns the job;
`doInstall` only starts the worker; `loop` observes completion; a timeout
must not drop ownership or report success. Home exit keeps the job until the
worker finishes. No user cancel of the transaction. A second install from
Home needs a serial entry around `M4xInstaller` install / uninstall / recover,
checked at every call site, or an explicit decision that a global mutex is
unsafe.

### Registry and Journal

`M4xRegistry.cpp` and `M4xInstallJournal.cpp` are not in this diff.
`M4xRegistry::save` still `serializeJson`s without an `overflowed` check, and
a failed rename removes the primary and falls back to a direct write
(lines 155–176). `durableWriteJournal` removes an existing `.tmp` before the
replacement write (line 157). `loadReconciledRaw`'s tmp promotion calls that
function (line 233), so a sole valid tmp can be deleted before the new bytes
exist. Not fixed.

### SdFat concurrency

No process-wide volume mutex was added. Search and the font scan can still
hold a directory across `delay(1)`. This build does not remount after boot.
A global lock was not invented.

### Long-run OOM

TTF, reader prefetch, and image decode still need a device loop. The QEMU
ping heap (`free_heap` 162368, `free_psram` 1769116) is one boot sample, not
that loop. No device plan was executed.

## Commit boundary

This report and the dirty source, tests, and the existing followup are the
commit. `firmware/.pio/` and both `firmware.bin` files are not. No second
branch was created.

## Install ownership and durable saves — 2026-09-27

The sections above describe `e0c9388` / `56e9389` and stay as that close-out.
The local image digests in "Builds reused" are that tree. They do not contain
the sources below. This section is the later pass. Commit: `INSTALL_COMMIT_SHA`.
Astra was not called. songzhangchi01 was not called. No subagent was started.
The audit is not finished. No real-device flash. No user SD.

### What this pass ran

Host regression `/tmp/m4-zxl-host-regress-2.sh` finished before either `pio`
run. `/tmp/m4-zxl-host-regress-2/summary.txt` records `EXIT:0` for astra,
batch, cache, cache-run, download, download-run, font, handoff, handoff-run,
http, install, lifecycle, paint, paint-run, registry, screen, sdretry, and
storage. The script prints `HOST_REGRESS_OK` only on that path, and the runner
exited 0. No firmware file was edited between that run and the two builds.

`test_m4_install_completion.py` printed:

- `actual installer worker / UI reclaims job immediately at done publication: PASS`
- `actual install ownership / timeout keeps the job until done: PASS`

`test_m4_registry_journal.py` printed:

- `registry/journal source order: PASS`
- `actual registry and journal writers / sole tmp, sync, rename, short write: PASS`

APP1 then QEMU, sequential, no overlapping `pio`:

| Env | Log | Result | RAM | Flash (limit 7,143,424) | SHA-256 |
| --- | --- | --- | --- | --- | --- |
| `murphy_m4` | `/tmp/m4-zxl-pio-app1.log` | SUCCESS 29.26 s, `APP1_RC:0` | 108,916 / 327,680 (33.2%) | 5,779,417 (80.9%) | `2a33f5e31da1ebf5c61a90e2b9e2c53fd338a0f1f5fdecf47f0f7aef58b3850c` |
| `murphy_m4_qemu_plugin` | `/tmp/m4-zxl-pio-qemu.log` | SUCCESS 29.34 s, `QEMU_RC:0` | 108,212 / 327,680 (33.0%) | 5,789,049 (81.0%) | `1a8dfd26cd3638998713bd109e147f743929a4dc720fccb79278f05516d3a017` |

Image sizes on disk: APP1 5,779,920 bytes, QEMU plugin 5,789,552 bytes. Both
stay under the APP1 slot. Neither file is committed.

Isolated smoke, second run, log `/tmp/m4-zxl-smoke-install.log`, session
`/tmp/m4-zxl-smoke-install-20260927`. `skip_build`, `fresh_sd` 64 MiB,
`no_hostfwd`. QEMU command uses
`-drive file=/tmp/m4-zxl-smoke-install-20260927/artifacts/murphy-sd.img,if=sd,format=raw`
and `-nic user,model=open_eth` with no `hostfwd` (port 18080 stays on mihomo).
The SD path is a regular 67,108,864-byte file; `/tmp` is `/private/tmp` on this
Mac. Composed flash SHA-256
`f5fc807cf3d6a632b69fb4b33f2d939db3e82a8f744173792ddb43c554b04a9e`.
The in-boot ping timed out once (`等待响应超时 (0.246s)`), then
`m4adb READY after 2 attempt(s)`. The script's own ping printed `PING_RC 0`
with `"protocol"`, `activity` Home, `sd_ok` true, `firmware`
`202608187-murphy-m4-qemu-plugin`. One boot sample: `free_heap` 162360,
`min_free_heap` 159944, `free_psram` 1769116, `largest_internal_block` 118772.
Frame `/tmp/m4-zxl-smoke-install-20260927/artifacts/smoke.pbm` is 48,011 bytes.
The script printed `SMOKE PASS` and `SMOKE_RC:0`, then `session stopped`.
QEMU pid 10795 is gone. Hardware `m4adb` on `/dev/cu.usbmodem101` (pid 47284)
was still running afterward. This smoke is boot readiness on an empty FAT
image. It does not open the drawer, drive install, or measure a long OOM loop.

An earlier script exit on the same image stopped at a bad check: `Path.resolve()`
turned `/tmp/...` into `/private/tmp/...` and the script treated that as an
escape. The guest had already reached Home. That process was stopped. The
`SMOKE PASS` line is only the re-run above.

### AppInstall ownership

`AppInstallActivity::doInstall` (lines 101–121) stores `"正在安装..."`, starts
`"M4xInstall"` at stack 12288, and returns. Create failure deletes the local
job, sets `"无法创建安装任务(内存不足)"`, and leaves `job_` null. A second
`doInstall` while `job_` is live returns without creating another task.

`loop` (lines 188–198) calls `observeInstallJob` before `M4RenderGuard` and
`render`. While `job_` is set it returns, so Back, confirm, and `onDone_` do
not retire the activity. A 60 s wait sets `resultMessage_` to `"安装超时"` and
`installTimedOut_`, and leaves `job_` in place. It does not mark `probe_.ok`.
When the worker release-stores `done`, `observeInstallJob` moves the result,
deletes the job, and replaces the timeout text with the real message.

`readyForDestruction` is false while `job_` is live and `done` is still false,
then it defers to the parent. `preventAutoSleep` is true while `job_` is live.
`onExit` does not delete the job, does not call `vTaskDelete`, and does not
delay. The destructor deletes the job only after an acquire sees `done`. If
destruction happens before that store, the destructor leaves the job allocated
so the worker is not writing into a freed object. The reaper is what is
supposed to wait. The display task `AppInstallUI` is gone; paint is the `loop`
path under `M4RenderGuard`.

### Registry and journal

`M4xRegistry::save` returns false on `overflowed`, a zero `serializeJson`, or
`written != out.size()` before `mkdir` or any file operation (lines 177–180).
The new bytes go to `/system/app_registry.json.tmp` through `writeAllTextExact`,
which refuses an existing path and removes only a file it just created when
sync, close, size, or the reopen check fails. Rename failure with the primary
still present leaves that primary and drops the tmp.

`durableWriteJournal` rejects an invalid body, writes `.tmp.part` first, and
does not remove a sole valid `.tmp` until the promoted body is reread and
matches. Host injection covered sync failure, short write, rename failure with
remove blocked, and a blocked stale `.tmp.part`.

Residual, still open: if sole-tmp promotion's `renameOrCopy(part, path)`
succeeds and the reread then fails (`M4xInstallJournal.cpp` lines 194–201),
the function removes `.part` and returns false. The new primary is already in
place, and the previous valid `.tmp` is still there. `decideLoad` prefers a
primary that still parses, so a corrupt-but-parseable body would hide the
good tmp. The sole tmp is intentionally not deleted on that path.

### Install gate

`InstallGateGuard` has a default constructor so `InstallGateGuard gate;`
compiles next to the deleted copy operations. `M4xInstaller::install` takes it
before `probe` (lines 706–717). `uninstall` takes it before
`M4xIsValidPackageId` (lines 836–841). Take failure returns `install_gate` /
`"无法开始安装"` or `errorOut = "install_gate"`. The wait is `portMAX_DELAY`.
The guard does not take `storageMutex_` or `gM4RenderMutex`. `ensureLayout`
does not take the gate: `install` reaches it through `probe`, and taking it
there would deadlock.

HTTP `handleUpload` still holds `StorageGuard(storageMutex_)` from
`M4FileTransferHttpRoutes.cpp` line 392 across `M4xInstaller::install` at line
492. That order is storage, then the gate. The UI worker takes only the gate.
The two locks do not cycle. A UI install stalls other file-transfer SD work
for the whole install. That stall is accepted. `main` uninstall and a debug
synchronous install block inside the same gate.

Still open: `probe` / `ensureLayout` / `recoverInterruptedInstalls` from
another task do not take the gate, so recovery can still run beside an
in-flight install. No process-wide SdFat mutex was added for that.

### SdFat concurrency — blocked, no code change

No volume mutex was added, and none should be until a lock order exists.
`FontManager::scanFonts` (`FontManager.cpp` lines 328–330) calls `openNext`
and then `delay(1)` every 32 entries while `fontDir` stays open.
`scanFiles` (`M4FileTransferHttpRoutes.cpp` lines 216–241) closes each child
and `yield()`s, and keeps `root` open across the next `openNextFile`. This
build does not remount after boot. The install gate does not cover these
scans. SdFat sources under libdeps were not edited.

### Long-run OOM — plan only, not executed

Do not flash this tree for the measurement. The operator backs up the user SD
first; this session does not mount or write that card.

1. TTF face-switch loop. Record `free_heap`, `min_free_heap`, `largest_internal_block`, and `free_psram` each switch.
2. Large txt page-forward for several minutes, same counters.
3. Image and cover decode until an allocation fails. The UI must fail closed. A reboot is a failure.

Pass on device: no panic, the heap returns to a floor after each scenario, and
the counters do not grow without a bound across repeats. The QEMU numbers
above are one boot. They are not that pass. No device PASS.
