# M4 Astra follow-up — library page cursor (2026-09-26)

Deputy repair on `feature/m4-stability-astra-20260926` at HEAD `2e75e2b140a642a4095f494ae96cb7313f1b6e9d`. Worktree `/private/tmp/m4-stability-astra-20260926`. Uncommitted Astra work already in the tree was left in place. This note covers one performance repair. The audit is not finished.

No `pio` build and no QEMU run in this pass. Build ownership stays with Astra. The published APP1 image from the earlier completion log predates both the uncommitted tree and this cursor change.

## Diagnosis

`MyLibraryActivity::scanDirectoryBatch` used to close the directory, open it again, and `seekSet(directoryCursor)` on every UI turn (at most 16 entries or 4 ms). Pinned SdFat 2.3.1 does not resume a reopened handle from the previous cluster.

`FatFile::seekSet` (`firmware/.pio/libdeps/murphy_m4/SdFat/src/FatLib/FatFile.cpp` lines 1166–1228):

- `pos == m_curPosition` returns without a FAT walk.
- `pos == 0` clears `m_curCluster`.
- Otherwise `nNew = (pos - 1) >> bytesPerClusterShift`.
- For a non-contiguous file, `nCur = (m_curPosition - 1) >> bytesPerClusterShift`. If `nNew < nCur` or `m_curPosition == 0`, the walk restarts at `rootDirStart()` / `m_firstCluster` and `fatGet` runs `nNew` times. A freshly opened file has `m_curPosition == 0`, so every seek to a later slot restarts at the first cluster. `(m_curPosition - 1)` also underflows, so `nNew < nCur` is true for the same reason.

`ExFatFile::seekSet` (`ExFatFile.cpp` lines 697–750) uses the same restart test at line 729. A contiguous exFAT directory jumps to `m_firstCluster + nNew` and does not walk the chain. FAT32 directories are not marked contiguous, so the chain walk is the device cost.

`FsFile::seekSet` dispatches to those two. `SdMan.open` defaults to `O_RDONLY` (`SDCardManager.h` line 70). `FatFile::openNext` reads the next directory cache from the live cursor (`FatFile.cpp` from line 644). It refuses an unaligned `curPosition() & 0x1F` and does not restart from cluster 0.

Closing and reopening therefore repeats the same prefix for every batch of a large filtered or no-match fragmented directory. With a 4096-byte cluster and one 32-byte slot per visible name, reopening every 4 host reads costs 847 FAT steps for 1000 entries and 96330 steps for 10000. That sum grows with the square of the entry count. A read-only `FsFile` kept across scheduler yields on one owner is sequential I/O. The shared block-cache race is unsynchronized use from another task. Close/reopen does not remove that race. The previous comment said otherwise.

## Repair

One owner-controlled read-only directory cursor for the in-progress visible page. `MyLibraryActivity.h` already declares `FsFile scanDirectory` (line 43) and was not edited.

| Symbol | What changed |
| --- | --- |
| `MyLibraryActivity::scanDirectoryBatch` | Opens and `seekSet(directoryCursor)` only when `!scanDirectory`. No `close` in this function. A successful partial batch stores the next aligned `curPosition()` and leaves the handle open. Error, cap, EOF, and an off-slot position still call `finishDirectoryPage()`. Each child entry is still `file.close()` before the turn returns. The 32-byte checks, 64-entry / 8 KiB cap, and resume-before-unconsumed-item assignment are unchanged. |
| `MyLibraryActivity::startDirectoryPage` | Still closes first, then sets `directoryPageStart = directoryCursor = position`. A new page or directory must not keep the previous seek position. |
| `MyLibraryActivity::finishDirectoryPage` | Still closes. Completion and error both drop the cursor. An error still clears `files` and returns before the `"< 回到首批 >"` / `"< 下一批 >"` rows. |
| `MyLibraryActivity::loop` | Back or cancel while `directoryLoading` closes the cursor, then clears the flag, then `returnToParent()`. |
| `MyLibraryActivity::onExit` | Closes the cursor, then clears `directoryLoading`, then deletes `renderingMutex`. |

`canMoveDirectory` (lines 114–132) and its paste call were already in the uncommitted tree. They were not edited. No process-wide SdFat lock and no SD API change.

## Host regression

`firmware/tests/test_m4_astra_stability.py` still compiles the real `loadFiles` / `startDirectoryPage` / `scanDirectoryBatch` / `finishDirectoryPage` / `selectDirectoryPage` window. The shim's `seekSet` counts calls and FAT steps with 4096-byte clusters and the same restart rule. `openNext` advances `millis` by 1, so a turn reads at most 4 entries. Long names are 500 `x` plus `%08zu.txt` (512 bytes): 15 fit in 8 KiB (`15 * 514 = 7710`, `16 * 514 = 8224`). Short names hit the 64-entry cap first.

The old `settle()` assertion required `!scanDirectory` after every turn. That encoded close/reopen and forced the next batch to `seekSet` from position 0. It now requires, while the page is loading: one open, one seek, the directory handle still open, and `openChildren == 0`. After the page the handle is closed. The 10000-entry `hidden.bin` case used to require the handle closed after a single unfinished turn. It now requires the cursor still open after that turn, then `startDirectoryPage(0)` closes it and a full settle finishes one empty non-error page with `opens == 1`.

`exercise` covers 0, 64, 65, 1000, and 10000 short names plus 10000 long names. For `count >= 1000` it requires `fatSteps * 2 < reopenEveryBatchCost`. `filtered` covers 1000 and 10000, short and long, all rejected `*.bin` names: `opens == 1`, `seekCalls == 1`, `fatSteps == 0` (the only seek is `seekSet(0)`), and the modeled reopen cost is strictly larger (`>= 64`). I/O fault, truncated name, pending select `07.txt`, unaligned `startDirectoryPage(1)`, and `skewAt = 4` still end closed with an empty error page. A runtime cancel runs the same two statements as `loop`'s Back path after one open. Source order outside the extract window asserts close-before-clear-before-`returnToParent` in `loop`, close-before-clear in `onExit`, no close inside `scanDirectoryBatch`, and a close in both `startDirectoryPage` and `finishDirectoryPage`. The directory-identity move guard and the 4000-early-return worker test still run.

Fresh stdout, exit 0:

```
actual directory move guard / FAT directory identities, aliases, I/O failure and siblings: PASS
actual task completion wrappers / 4000 early returns: PASS
actual directory scanner / 1k+10k+10k long names+I/O faults: PASS
```

The harness links with `-fsanitize=address,undefined`.

## Residual cost across pages

Inside one page, `openNext` steps the current cluster. Across pages, `startDirectoryPage` closes the handle, so the first batch of the next page seeks from position 0 and walks the prefix once. Modeled steps (one slot per visible name, 4096-byte clusters):

| Case | Pages | One seek per page | Reopen every 4 reads |
| --- | ---: | ---: | ---: |
| 1000 short names, 64/page | 16 | 49 | 847 |
| 10000 short names, 64/page | 157 | 6006 | 96330 |
| 1000 long names, 15/page | 67 | 226 | 847 |
| 10000 long names, 15/page | 667 | 25693 | 96330 |
| 1000 or 10000 filtered, any name length | 1 | 0 | 847 or 96330 |

Page-boundary walks are still quadratic in the number of pages for a fragmented directory. They are about 16× fewer steps than per-turn reopen for short names and about 3.75× fewer for 15-entry long-name pages. The `* 2` test bound is the one that holds for both; `* 4` fails the long-name page cost. Real long-file-name entries occupy several 32-byte slots, so a device walk is larger than these counts. The one-open-per-page invariant does not depend on that model.

The live cursor now spans the whole in-progress page, including `render()` in the same turn. That lengthens the window of the existing unsynchronized-SdFat race. It does not add a lock.

`searchFilesRecursive` (`MyLibraryActivity.cpp` lines 139–151) still holds the parent directory across `delay(1)` after the child is closed. That is outside this repair.

## Observations in the existing uncommitted tree

These were read and not edited.

- `TxtReaderActivity::persistProgressSnapshot` (lines 4803–4808) removes `progress.dat` and then renames `progress.tmp` onto it. If remove succeeds and rename fails, the dat name is gone and the synced temp remains. `loadProgress` (lines 4866–4870) reads tmp when dat fails, then `saveProgress()` migrates it. Legacy `progress.bin` is no longer removed on this save path; load uses it only when dat and tmp both fail to read.
- `ClearCacheActivity` (lines 156–159) treats a truncated root `getName` as fatal for the whole `/.crosspoint` scan: `failedCount` increments and the loop breaks, so later cache directories stay. A truncated child name (line 182) stops only that cache directory. `root.getError()` also increments `failedCount`. The UI becomes `FAILED`. `m4ReaderCacheBusy()` still refuses the clear while deferred readers or Home cover workers exist, and this path does not remount.
- `main.cpp` lines 1030–1044 retry `SdMan.capabilityProbe` on the already mounted volume. The probe closes its own handles and does not call `begin()`. The earlier card-init failure (lines 991–1016) still calls `SdMan.begin()` after a tap or a fresh Confirm. A stuck SDMMC after a successful mount can be tapped indefinitely on the read-retry screen.
- `HttpDownloader::downloadBody` refuses the transfer when `dest + ".m4-download.bak"` already exists (line 85). `M4DownloadCommit::commit` failure keeps tmp and bak (lines 104–108), and a later request hits that same refusal. `BoundedWriteStream::write` calls `delay(1)` after every successful or failed chunk (line 56). `M4NativeProviderHeavyGate::Lock` is held around `fetchUrlBounded` and each `downloadToFile*` entry (lines 120, 193, 232, 271).
- `docs/M4_ASTRA_STABILITY_AUDIT.md` section 2 still cites the directory-cut lines as 1239–1256. After the uncommitted `canMoveDirectory` insert, the paste site is later in the file. Section 2 of this follow-up and audit line 110 describe the identity guard that is actually in the tree. The audit file was not edited.
- The workspace memory note `m4-firmware-stability.md` still says the library listing closes between batches. It was not edited. It is stale relative to this repair.

## Not done

No firmware image, no isolated QEMU directory scenario, no commit, no push. Directory paging, sorted pages, parent crumbs, selection, the 32-byte slot checks, and the move-identity guard remain as they were apart from the cursor lifetime above. The rest of the stability audit is still open.

## ScreenBridge worker lifetime (2026-09-27)

Same worktree and same HEAD `2e75e2b140a642a4095f494ae96cb7313f1b6e9d`. No `pio`, no QEMU, no commit. BatchInstall, AppList, AppInstall, and Registry/Journal were not edited.

### Diagnosis

`ScreenBridgeController::stopWorker` waited 100×20 ms and then called `M4Psram::deleteTask(handle)` from the UI task. `M4Psram::deleteTask` is `vTaskDeleteWithCaps` and does not run C++ destructors. The worker can be inside `request()` (`timeoutMs = 20000`) or an unsliced `vTaskDelay` of 1800/900/700 ms while it holds `mu_` or the HTTP stack. `~ScreenBridgeController` called `stopWorker`, and `NativeAppActivity::onExit` called `controller_.reset()`, so the activity thread both blocked and force-deleted the worker.

`main.cpp` already retires an activity after `onExit` and deletes it only when `readyForDestruction()` is true. `ActivityWithSubactivity::readyForDestruction` requires the live child and every retired child. `AppRuntimeActivity` is the existing pattern: AND the parent result with the owner flag. NativeApp did not need a `main.cpp` change.

### Repair

| Symbol | What changed |
| --- | --- |
| `M4NativeUi::Controller::requestStop` | New default, empty body. Not pure virtual. |
| `M4NativeUi::Controller::readyForDestruction` | New default, returns true. Not pure virtual. `BaseController` and `ProviderController` stay concrete and immediately destructible. |
| `NativeAppActivity::onExit` | Still calls `ActivityWithSubactivity::onExit()`, then `controller_->requestStop()` when `controller_` is set, then `document_ = {}`. Does not `controller_.reset()`. |
| `NativeAppActivity::readyForDestruction` | `ActivityWithSubactivity::readyForDestruction() && (!controller_ \|\| controller_->readyForDestruction())`. |
| `ScreenBridgeController::requestStop` | Release-stores `stop_`. No mutex, no wait, no `deleteTask`. Superseded by the exit-boundary section below: the first version locked `mu_`. |
| `ScreenBridgeController::readyForDestruction` | On `ARDUINO_ARCH_ESP32`, `try_lock` on `mu_` and then `task_ == nullptr`. A busy mutex returns false. Otherwise true (no worker). |
| `ScreenBridgeController::delayUnlessStopped` | Acquire-loads `stop_` before each slice of at most 20 ms, and once more after the last slice. Returns false when stop was requested. Does not lock. |
| `ScreenBridgeController::publishWorkerDone` | Under `mu_`, `task_ = nullptr`. The `lock_guard` unlocks. Then `M4Psram::deleteTask(nullptr)`. That call is the last statement. On device it does not return and does not use `this`. |
| `ScreenBridgeController::workerLoop` | Idle wait is `delayUnlessStopped(40)`. After the loop the only statement is `publishWorkerDone()`. |
| `ScreenBridgeController::run` | The 1800/900/700 ms waits return immediately on cancel and skip the trailing status update. The status update itself returns if `stop_` is set. |
| `loadFeed` / `loadComments` | The 900 ms and 700 ms polls use `delayUnlessStopped` and return false on cancel. |
| `~ScreenBridgeController` | Empty. `stopWorker` is gone. |

`startWorker` still returns when `task_` is set or `stop_` is already stored, so a later `pollAsync` does not create another worker after stop. Create failure still sets `status_` to `无法启动屏幕桥任务`, leaves `task_` null, and therefore stays ready. `request()` still uses `timeoutMs = 20000`. Its cancel lambda acquire-loads `stop_` and does not lock. `M4ScreenBridgeController.h` was not changed.

Publication order: the worker writes `task_ = nullptr` while it holds `mu_`, unlocks, and only then calls `deleteTask(nullptr)`. `readyForDestruction` `try_lock`s `mu_` before it reads `task_`. A failed try returns false and does not observe the flag. A true result happens only after that unlock. The reaper may destroy the object once it has observed that flag. The worker must not touch `this` after the unlock.

### Host regression

`firmware/tests/test_m4_screen_bridge_lifecycle.py` copies the real `requestStop`, `readyForDestruction`, `pollAsync`, `startWorker`, `delayUnlessStopped`, `publishWorkerDone`, `workerLoop`, and `request` bodies, plus the real `NativeAppActivity::onExit` and `readyForDestruction`. It does not compile production `run()` (ArduinoJson / SD / Wi-Fi). The stub `run()` calls the extracted `request()` and `delayUnlessStopped`. `std::mutex` is replaced with a tracking mutex only inside that paste. `createTask` records the thread and does not run it: `startWorker` holds `mu_` across `createTask`, so an inline start would deadlock the shim. The device scheduler can run the new task on the other core; that task waits for `mu_` and does not deadlock once `startWorker` returns.

The network shim blocks in 20 ms slices for 2500 ms unless the real cancel lambda sees `stop_`. It holds a gate object and a 256-byte string and releases both when it returns. After `deleteTask(nullptr)` the test destroys the object and then joins the thread, under `-fsanitize=address,undefined`.

Fresh stdout, exit 0:

```
delay cancel: request_stop_us=0 elapsed_ms=32
network cancel: slices=3 remaining_ms=2440 request_stop_us=0 stop_to_ready_ms=19 total_ms=84
normal release: request_slices=0 elapsed_ms=54 gates=1/1 bufs=1/1
actual screen bridge lifecycle / cancel, publish, native app ready: PASS
```

`request_stop_us=0` is below this host's 1 µs clock step. The 5000 ms delay returned in 32 ms. The 2500 ms network wait was cancelled with 2440 ms still remaining; stop-to-ready was 19 ms and the whole case was 84 ms. The success path constructed and destroyed one gate and one buffer (`1/1`) and did not enter the long loop. Source checks also require: hooks are not pure; `onExit` keeps the parent call and does not reset; `publishWorkerDone` clears `task_` inside the lock and calls `deleteTask(nullptr)` only after that lock's closing brace, with no member use after the call; `workerLoop` ends at `publishWorkerDone()`; the file contains one `M4Psram::deleteTask(` and one `vTaskDelay(`; the destructor does not lock, wait, or delete a task. The runtime also covers create failure, no worker, a second `requestStop`, `pollAsync` after stop (no second `createTask`), a controller that does not override the new hooks, and NativeApp ready staying false until both the parent and the controller are ready.

### Residual

`ScreenBridgeActivity::onExit` (`ScreenBridgeActivity.cpp` lines 68–70) still calls its own `stopWorker` (lines 267–283), which loops on `vTaskDelay(20)` until `task_ == nullptr`. That path waits on the UI thread instead of force-deleting. It is a different class and was not in this whitelist.

`M4NativeProviderHttp::perform` still has one unsliced `delay(600)` on the zero-byte retry (`M4NativeProviderHttp.cpp` line 185), between cancel checks at lines 181 and 186. `requestSmall` forwards the cancel callback into `perform`. A socket read ends when `M4HttpTransport::requestToSink` honors `cancelShim`, or when the HTTP timeout fires. This batch does not change that transport. `request()` can therefore still sit inside one transport read or that 600 ms retry after `stop_` is set. It will not be deleted out from under that read.

`startWorker` still holds `mu_` for the `createTask` call. That is the previous code. The new task blocks on `mu_` until the call returns.

The empty destructor does not tolerate a delete that ignores `readyForDestruction`. The reaper and `ActivityWithSubactivity` already keep a not-ready object; the parent destructor leaks a not-ready child rather than deleting it. A direct `delete` of a live `ScreenBridgeController` would still be a use-after-free.

No firmware image and no commit for this batch. The audit is not finished.

## ScreenBridge exit boundary (2026-09-27)

Same worktree and same HEAD `2e75e2b140a642a4095f494ae96cb7313f1b6e9d`. No `pio`, no QEMU, no commit. BatchInstall, AppList, AppInstall, and Registry/Journal were not edited. The three items below are the exit-boundary follow-up on the lifecycle batch. `ScreenBridgeActivity` was not edited.

### HTTP shutdown while a deferred worker still holds HeavyGate

`onGoHomeAnimated` calls `exitActivity()` before `releaseM4HomeBoundaryResources()`. `exitActivity()` links the NativeApp into `deferredActivities`. The helper used to call `M4HttpTransport::shutdown()` whenever `m4HomeBoundaryWorkersBusy()` was false. ScreenBridge is not in that set. `shutdown()` is `sessionEnd()`, which takes `M4NativeProviderHeavyGate::mutex()` and would block the UI for as long as the worker's in-flight HTTP call holds the gate.

`releaseM4HomeBoundaryResources` now skips `shutdown()` when `providerBusy || hasDeferredActivities()`. The provider-cancel calls and the existing 450 ms provider wait stay. The stale comment that said activities join their workers in `onExit()` is gone. No second join was added.

`loop()` already does the later shutdown. After `reapDeferredActivities()`, `gM4PendingTransientReset && !hasDeferredActivities()` and `!m4HomeBoundaryWorkersBusy()` still guard `M4HttpTransport::shutdown()` and `M4Memory::resetTransient()`. That block was not replaced.

### `requestStop` / `readyForDestruction` must not block on `mu_`

`loadComments` holds `mu_` across the `comments_` duplicate scan and the `commentsText_` build. Those hooks previously took the same mutex, so the UI blocked for that critical section. `stop_` is now `std::atomic<bool>`. `requestStop` is a release store and does not lock. Every `stop_` read is an acquire load: `startWorker` (still under `mu_` for `task_` / `status_`), `delayUnlessStopped`, `workerLoop`, the trailing status update in `run`, the `request()` cancel lambda, and the `ensureEndpoint` Wi-Fi cancel lambda. `publishWorkerDone` still clears `task_` under `mu_` and then calls `deleteTask(nullptr)`. `readyForDestruction` uses `std::try_to_lock`; if `loadComments` (or any other holder) owns `mu_`, it returns false and does not read `task_`. The comment-cache scan itself is unchanged.

### Last delay slice

`delayUnlessStopped` used to return true after the final `vTaskDelay` without looking at `stop_` again. A stop during that slice still fell through into `loadFeed`, `loadNote`, or `loadComments`. The function now acquire-loads `stop_` after the loop and returns false when the flag is set, so those `if (!delayUnlessStopped(...)) return;` sites do not start the next request.

### Host regression

`firmware/tests/test_m4_screen_bridge_lifecycle.py`, same command as the lifecycle batch. It still compiles the real lifecycle methods, and it now also compiles the real `hasDeferredActivities`, the real `releaseM4HomeBoundaryResources`, and the real `gM4PendingTransientReset` condition through its `M4HttpTransport::shutdown()` call. Provider `busy()` / `cancel()` and `millis` / `vTaskDelay` are host stubs. The deferred flag is the real pointer check.

Fresh stdout, exit 0:

```
delay cancel: request_stop_us=0 elapsed_ms=32
network cancel: slices=3 remaining_ms=2440 request_stop_us=0 stop_to_ready_ms=22 total_ms=85
normal release: request_slices=0 elapsed_ms=52 gates=1/1 bufs=1/1
hold mu: request_stop_us=0 ready_us=0 ready_while_held=0
last slice: returned_false=1 delay_entries=1 elapsed_ms=27
last slice follow-on: requests=0
boundary deferred: shutdowns=0 elapsed_ms=0 delay_entries=0
boundary clear: pending_shutdowns=1
actual screen bridge lifecycle / cancel, publish, native app ready: PASS
```

`hold mu` is the worker holding `mu_` for up to 2 s. `requestStop` and `readyForDestruction` both returned below this host's 1 µs step (`0`), ready was false while the lock was held, and ready became true only after the worker unlocked and published. `last slice` set stop from inside the single 20 ms `vTaskDelay`; the real `delayUnlessStopped(20)` returned false (`delay_entries=1`). The worker path that would issue the next request stayed at `requests=0`. `boundary deferred` is `hasDeferredActivities()==true` and `m4HomeBoundaryWorkersBusy()==false`: shutdown count 0, no provider wait (`delay_entries=0`, elapsed 0 ms). The same run then checks the helper's idle `else` (shutdown once), a busy provider set (shutdown 0, fake millis advanced by at least 450), the pending-reset path while deferred is still set (shutdown 0), and that same pending-reset path after deferred and providers are both clear (shutdown 1).

### Residual

`ScreenBridgeActivity::stopWorker` (lines 267–283) still joins on the UI thread. `M4NativeProviderHttp::perform` still has `delay(600)` at line 185, and a socket read still ends only when the transport honors cancel or the HTTP timeout fires. `startWorker` still holds `mu_` across `createTask`. `readyForDestruction` returns false for the whole `loadComments` critical section, so destruction waits until that scan finishes and the worker publishes; the scan itself is not cancelled mid-loop. The empty destructor still assumes the reaper honors `readyForDestruction`.

No firmware image and no commit. The audit is not finished.

## BatchInstall inbox walk (2026-09-27)

Same worktree `/private/tmp/m4-stability-astra-20260926`, same HEAD `2e75e2b140a642a4095f494ae96cb7313f1b6e9d`. No `pio`, no QEMU, no device, no commit. AppList, AppInstall, Registry, and Journal were not edited. This batch is `BatchInstallActivity.cpp` / `.h` plus `firmware/tests/test_m4_batch_install_scan.py`.

### One parent cursor, one path, destructors before `done`

`batchTaskTrampoline` no longer keeps `scanInbox()`'s vector until `vTaskDelete`. It calls `runInboxBatch`, release-stores `job->done`, and then `vTaskDelete(nullptr)` with no further job, path, file, or string access. `runInboxBatch` opens `/apps_inbox` once. Each iteration holds one `FsFile` child, copies the name, closes and destroys that child, then calls `processInboxPackage` on one path string. The registry vector from `M4xRegistry::load()` is scoped inside `processInboxPackage` and dies before install or remove. `root` and the path string die when `runInboxBatch` returns, before `done` is published. The directory is not reopened per package.

`kInboxNameBytes` is `255 * 3 + 1` (766), defined inside `runInboxBatch`. `getName` uses that buffer. A zero return, a length that fills the buffer, or a missing NUL is a name failure: a file counts `failed`, is not added to `total` or `processed`, is not deleted, and the walk continues. A directory name failure is not a package failure. Non-`.m4x` names (`isM4xName`, case-insensitive suffix) are ignored. Probe failure, install failure, and a failed remove after a successful install still keep the package. `m4xInboxBatchDecide` is unchanged: an older or equal installed version is `SkipDelete` and is removed on success. `installed + skipped + failed` can exceed `total` because a root or name failure increments `failed` without discovering a package.

A root open failure, a root that is not a directory, and `openNext` false with `root.getError() != 0` each add one failure, clear `scanning`, and stop. They are not an empty inbox. Clean EOF (`openNext` false and `getError() == 0`) is not a failure. `scanning` is cleared once the root is open, before the walk, so the in-progress line is not a finished-scan total.

### Refresh

The dedicated display task, `updateRequired_`, and `renderingMutex_` are gone. `loop` snapshots the seven job fields and calls `render` only when the snapshot changed and either this is the first paint, `done` is set, or at least `kIntervalMs` (500) has passed since the last paint. While the job is not done, Back / Confirm / tap do not leave. After done, that exit path is unchanged. In progress the line is `已处理 %d，已发现 %d` (processed, discovered so far). `正在扫描 apps_inbox…` is only while `scanning` is still true. The completion line is still `安装 %d，跳过 %d，失败 %d`.

### Exit still waits

`onExit` still calls `ActivityWithSubactivity::onExit()`, then blocks on `job_->done` with 20 ms delays, then `delete`s the job. There is no process-wide install mutex, so this screen still cannot leave while the transaction runs. Concurrent installs were not enabled. That wait stays for the later ownership fix.

### Host regression

`cd /private/tmp/m4-stability-astra-20260926 && python3 firmware/tests/test_m4_batch_install_scan.py`

The harness compiles the real `isM4xName`, `processInboxPackage`, `runInboxBatch`, `batchTaskTrampoline`, `onExit`, `loop`, and `render`, plus the real `m4xInboxBatchDecide` and `M4xPaths::kInbox`. Probe, install, registry, SdFat, and UI are host stubs. The directory double advances one parent cursor, counts a second `SdMan.open` while the cursor is nonzero as a rewind, and skips `.`, `..`, and deleted slots inside `openNext` without returning a closed file. ASan and UBSan, exit 0. macOS ASan does not include LeakSanitizer; the live-count asserts are the destructor check.

Fresh stdout:

```
mixed 1000: m4x=400 installed=400 failed=0 skipped=0 opens=1 reads=1000 delays=1000 left=600
mixed 10000: m4x=4000 installed=4000 failed=0 skipped=0 opens=1 reads=10000 delays=10000 left=6000 ms=102
long-name: installed=1 left=0 bytes=604
name-fail: failed=1 kept=1 later_installed=1
read-error: installed=2 failed=1 visited_c=0
eof-empty: failed=0 total=0
root-open-fail: failed=1 total=0
root-not-dir: failed=1
delete-cursor: visited=7 rewinds=0 opens=1 installed=2 skipped=1 failed=3 kept=4
dtors: fs=9 strings=86 apps=25 self_delete=1
render: initial=1 same=1 limited=1 later=2 done=3
exit-wait: elapsed_ms=58
actual batch install scan: PASS
```

1k and 10k are mixed directories, `.txt`, `.zip`, `.m4x`, and `.M4X` (one open, one read per entry, no rewind, non-packages retained). The 604-byte UTF-8 name installs and is removed. A name longer than 765 bytes fails `getName`, stays on disk, and the following `later.m4x` still installs. A read error at slot 2 stops without visiting or deleting `c.m4x`. Empty EOF is not a failure. The delete-current fixture visits `ok-a.m4x`, `bad-probe.m4x`, `note.txt`, `old-ver.m4x`, `bad-install.m4x`, `bad-remove.m4x`, `ok-b.m4x` once (9 slot reads including `.` and `..`, 0 rewinds, 1 open). Success and the older version are removed; probe failure, install failure, remove-after-install failure, and `note.txt` stay. `self_delete=1` ran only after live `FsFile` / string / registry-app counts were 0, and those destructors did not observe `done`. Fifteen unchanged loops and a progress change inside the 500 ms window did not add a frame. Completion painted at the same `millis` as the previous frame (`done=3`). `onExit` waited 58 ms for a worker that published `done` after 40 ms, then cleared `job_`, and did not call `vTaskDelete`.

### Residual

`onExit` still blocks the UI until this batch's worker publishes `done`. AppList's bounded force-delete, AppInstall's 60 s abandoned `InstallJob`, and the Registry / Journal serialize and sole-tmp gaps were not touched.

No firmware image and no commit. The audit is not finished.

## AppList display retirement (2026-09-27)

Same worktree `/private/tmp/m4-stability-astra-20260926`, same HEAD `2e75e2b140a642a4095f494ae96cb7313f1b6e9d`. No `pio`, no QEMU, no device, no commit. AppInstall, Registry, and Journal were not edited. BatchInstall `onExit` still waits for `job_->done`. This batch is `AppListActivity.cpp` / `.h` plus `firmware/tests/test_m4_app_list_lifecycle.py`.

### No force-delete

`onExit` (lines 509–514) only release-stores `childScreenOwned_` and `exitDisplayTask_`, then calls `ActivityWithSubactivity::onExit()`. It does not delay, join, `deleteTask`, or `vSemaphoreDelete`. `readyForDestruction` (header lines 40–43) is `displayTaskExited_` acquire-load AND `ActivityWithSubactivity::readyForDestruction()`. The default is `displayTaskExited_{true}` (header line 106), so a `pendingSubActivity` that never reached `onEnter` is destructible. `~AppListActivity` (lines 516–528) deletes `renderingMutex_` then `reloadLock_` only after that acquire is true. It does not delete the task.

The display loop publishes at the top, with no locks held (lines 241–249): `displayTaskHandle_ = nullptr`, then `displayTaskExited_.store(true, release)`. That store is the last `this` access. The following `Serial.printf`, `uxTaskGetStackHighWaterMark(nullptr)`, and `M4Psram::deleteTask(nullptr)` do not touch members. `onEnter` (lines 480–507) returns before `applyCachedDrawer` / `reload` / `createTask` if either `xSemaphoreCreateMutex` fails, and leaves `displayTaskExited_` true. It stores the flag false only after both mutexes exist, and stores it true again if `createTask` fails. A failed mutex is not used to run `reload` or the worker.

### One snapshot copy

The by-value `AppListFrameSnapshot snapshot` and the later `snapshot_ = snapshot` are gone. Under `renderingMutex_` the loop aliases `AppListFrameSnapshot& snapshot = snapshot_` and assigns `snapshot.items = items_` and `snapshot.apps = apps_` (lines 262–267), then `xSemaphoreGive` (line 271), then `M4RenderGuard renderGuard(gM4RenderMutex)` (line 277), then `render()`. `render()` still reads `snapshot_` inside that guard. The local mutex is not held while taking the global guard. The else re-arm still takes the local mutex while the guard is in scope.

### Home M4 path

`HomeActivity.cpp` was not edited. The M4 `displayTaskLoop` (lines 1591–1604) takes Home's own `renderingMutex`, calls `render(ctx)` while holding it, then gives it. That `render` calls `renderSnapshotScene(ctx)` (line 1687). The file has no `gM4RenderMutex` and no `M4RenderGuard`, so there is no global-inside-local order to reverse.

### Host regression

`cd /private/tmp/m4-stability-astra-20260926 && python3 firmware/tests/test_m4_app_list_lifecycle.py`

The harness compiles the real `taskTrampoline`, `displayTaskLoop`, `sameInstalledApp`, `sameDrawer`, `applyCachedDrawer`, `reload`, `onEnter`, `onExit`, the destructor, and the header `readyForDestruction`. Semaphores are `std::timed_mutex`. `deleteTask(nullptr)` throws after checking that no lock is held, so a publish-time delete of the activity cannot hide a member access. ASan and UBSan, exit 0. macOS ASan does not include LeakSanitizer; `sem_live=0` is the leak check.

Fresh stdout:

```
display-hold: on_exit_ms=0 ready=0 foreign_deletes=0 sem_deletes=0 locks=1
display-release: ready=1 sem_deletes=2 self_deletes=1
reload-hold: on_exit_ms=0 ready=0 foreign_deletes=0 sem_deletes=0 locks=1
reload-release: ready=1 sem_deletes=2 self_deletes=1
never-enter: ready=1 sem_deletes=0 sem_live=0
mutex-both-fail: ready=1 reloads=0 tasks=0 sem_deletes=0 sem_live=0
mutex-first-fail: ready=1 reloads=0 tasks=0 sem_deletes=1 sem_live=0
mutex-second-fail: ready=1 reloads=0 tasks=0 sem_deletes=1 sem_live=0
task-fail: ready=1 reloads=1 tasks=1 sem_deletes=2 self_deletes=0
publish-uaf: on_exit_ms=0 deleted_in_printf=1 self_deletes=1 sem_deletes=2 foreign_deletes=0
dirty-frame: copy_assigns=2 copy_ctors=0 items=4 apps=2
actual app list lifecycle: PASS
actual app list lifecycle: PASS
```

A worker blocked inside the snapshot assign (`renderingMutex_` held) and a worker blocked inside `M4xRegistry::load` (`reloadLock_` held) both saw `onExit` return in 0 ms with `ready=0`, `foreign_deletes=0`, and `sem_deletes=0`. After the lock was released, `ready=1` and the destructor deleted both mutexes (`sem_deletes=2`) with one self-delete. Never-entered destruction deleted nothing. Mutex create failure ran no reload and no task; the surviving semaphore was deleted. Task create failure ran one reload, deleted both semaphores, and did not self-delete. The printf hook deleted the activity before `deleteTask(nullptr)`; ASan stayed clean. One dirty frame copy-assigned the item vector and the app vector once each (`copy_assigns=2`, `copy_ctors=0`).

Existing source contracts, not edited: `firmware/tests/native_app/test_app_drawer_handoff.cpp` printed `app drawer handoff contracts: ALL PASS`. `firmware/tests/native_app/test_m4_plugin_entry_paint.cpp` printed `PLUGIN_ENTRY_PAINT_CONTRACT_OK`.

### Residual

AppInstall's 60 s abandoned `InstallJob`, and the Registry / Journal serialize and sole-tmp gaps, were not touched. BatchInstall `onExit` still blocks until its worker publishes `done`. There is still no process-wide install mutex.

No firmware image and no commit. The audit is not finished.

## AppList display ownership (2026-09-27)

Same worktree `/private/tmp/m4-stability-astra-20260926`, same HEAD `2e75e2b140a642a4095f494ae96cb7313f1b6e9d`. No `pio`, no QEMU, no device, no commit. Home was checked and not edited. AppInstall, Registry, and Journal were not edited. BatchInstall `onExit` still waits for `job_->done`. The deferred-retirement behavior in the previous section stays: `onExit` only signals, and the destructor deletes the two mutexes only after the owner publishes exit. The "One snapshot copy" paragraph above describes the worker submit that this change removed. The worker no longer renders.

### Why a Home-only lock is not the fix

`HomeActivity::displayTaskLoop` (lines 1591–1604) takes only Home's local `renderingMutex` and calls `render(ctx)` while holding it. `HomeActivity.cpp` contains no `gM4RenderMutex` and no `M4RenderGuard`. `GfxRenderer::displayBuffer` takes no global render lock. MyLibrary and the other main-thread destinations do not share one either. Before this change, `onExit` could return while the AppList worker was still inside `render()`, and `exitActivity` could then construct Home or the library on the same renderer. Locking only Home would have left the other destinations in that race. No process-wide render lock was added, and `M4RenderGuard.h` was not edited.

### Paint is on AppList's main loop

`submitDirtyFrame` (lines 348–387) runs from `loop()` (lines 735–737) only after the `if (subActivity)` block has returned (line 732) and only when `exitDisplayTask_` is clear. That call is before `mappedInput` (line 739), so a Back that synchronously builds the next page cannot paint AppList on top of it. `pumpSubActivityFrame` may clear `subActivity` inside the child block, but that block still returns, so the parent paints on the next `loop()` call.

`submitDirtyFrame` returns immediately when `subActivity` is set, `exitDisplayTask_` or `childScreenOwned_` is set, or `updateRequired_` is clear. Otherwise it takes `renderingMutex_` for 100 ms, rechecks those gates, aliases `AppListFrameSnapshot& snapshot = snapshot_`, copy-assigns `snapshot.items = items_` and `snapshot.apps = apps_` once each, clears `updateRequired_`, and calls `M4FooterTouchPolicy::setMask` before `xSemaphoreGive`. It then constructs `M4RenderGuard`. `render()` and `showedDrawer_.store(true, release)` run only when the guard owns the mutex and the child, exit, and `subActivity` gates are still clear. The else branch re-arms `updateRequired_` by taking the local mutex while the guard is still in scope, so a held guard stays global-then-local. The local mutex is not held across the guard constructor. There is still one vector copy of items and one of apps. `reload()` was not moved onto the UI thread. Directory and icon I/O stay in the worker.

### Worker

`displayTaskLoop` (lines 239–287) keeps the exit publication exactly: `displayTaskHandle_ = nullptr`, then `displayTaskExited_.store(true, release)`, then printf / stack watermark / `M4Psram::deleteTask(nullptr)`. It does not call `render`, `M4RenderGuard`, `setMask`, `clearScreen`, or read `subActivity`. The cache check requires `verifyDrawerCache_`, `showedDrawer_`, and `!childScreenOwned_`. The `mode_ == 0` read is between `xSemaphoreTake(renderingMutex_)` and that take's `xSemaphoreGive`. `reload(true)` still does the registry load and icon I/O. If it returns changed and the activity has not exited, the worker sets `updateRequired_` only when the child bit and the exit bit are both clear. An unchanged drawer in mode 0 clears `verifyDrawerCache_`. A dialog (`preserveDialog && mode_ != 0`) leaves the verify bit set and returns false. `reload` (lines 389–496) returns false on `exitDisplayTask_` before I/O and again immediately after `M4xRegistry::load()`, before `addBuiltin`. It does not call `setMask`. `M4ReturnCache::rememberDrawer` does not draw or write the footer. `applyCachedDrawer` still writes the footer, and that call is on the UI thread inside `onEnter`.

`showedDrawer_` and `verifyDrawerCache_` are `std::atomic<bool>` (header lines 113–116) with release stores and acquire loads. `reloadChanged_` and `reloadPreserveDialog_` are gone. `reload(bool preserveDialog = false)` keeps the default on the header only (line 126). `mode_` is not atomic. The dialog `mode_ = 1` write in `loop()` (lines 773–778) is now inside `xSemaphoreTake(renderingMutex_)`. Other UI `if (mode_ ==` reads stay unlocked; that race is the pre-existing one and was not spread into a lock on every read. Nobody takes `renderingMutex_` and then `reloadLock_`. The worker takes `reloadLock_` first.

### Host regression

`cd /private/tmp/m4-stability-astra-20260926 && python3 firmware/tests/test_m4_app_list_lifecycle.py`

ASan and UBSan, exit 0. macOS ASan does not include LeakSanitizer; `sem_live=0` is the leak check. The harness calls `submitDirtyFrame` on the test thread. The worker is blocked inside `M4xRegistry::load` for the cross-page case and inside the reload `apps_ = apps` assign for the display-lock case.

Fresh stdout:

```
display-hold: on_exit_ms=0 ready=0 foreign_deletes=0 sem_deletes=0 locks=2
display-release: ready=1 sem_deletes=2 self_deletes=1
cross-page: on_exit_ms=0 ready=0 foreign_deletes=0 sem_deletes=0 locks=1
cross-page-resume: renders=1 footer=2 guards=1 home_draws=1
cross-page-release: ready=1 sem_deletes=2 self_deletes=1
never-enter: ready=1 sem_deletes=0 sem_live=0
mutex-both-fail: ready=1 reloads=0 tasks=0 sem_deletes=0 sem_live=0
mutex-first-fail: ready=1 reloads=0 tasks=0 sem_deletes=1 sem_live=0
mutex-second-fail: ready=1 reloads=0 tasks=0 sem_deletes=1 sem_live=0
task-fail: ready=1 reloads=1 tasks=1 sem_deletes=2 self_deletes=0
publish-uaf: on_exit_ms=0 deleted_in_printf=1 self_deletes=1 sem_deletes=2 foreign_deletes=0
dirty-frame: copy_assigns=2 copy_ctors=0 items=4 apps=2
frames: initial=1 selection=2 cache=3 apps=1 id=changed
child-skip: during_child=0 after_child=1
actual app list lifecycle: PASS
actual app list lifecycle: PASS
```

`onExit` while the worker held both mutexes inside the reload publish returned in 0 ms (`locks=2`, `ready=0`, no deletes). After the assign was released, destruction deleted both mutexes. `onExit` while `reloadLock_` was held inside `load` also returned in 0 ms (`locks=1`). `simulateHomeDraw` ran before the worker resumed (`home_draws=1`). After resume, `renders`, `footer`, and `guards` stayed at 1, 2, and 1: the worker did not draw, submit, or write the global footer. The initial frame, the selection dirty frame, and the cache-update frame each rendered once (`frames` 1, then 2, then 3). The cache frame published one app, id `changed`. A second submit of a clean frame did not render. With `childScreenOwned_` set, and again with `subActivity` set, `submitDirtyFrame` rendered 0 times and left the dirty bit set. After both were cleared it rendered once. Never-enter, mutex-create failure, task-create failure, and the publication-order delete still match the previous retirement checks. One dirty frame still copy-assigns each vector once (`copy_assigns=2`, `copy_ctors=0`).

`firmware/tests/native_app/test_app_drawer_handoff.cpp` and `firmware/tests/native_app/test_m4_plugin_entry_paint.cpp` were updated to the new split and recompiled. The worker span now asserts the absence of `M4RenderGuard`, `render()`, `setMask`, `snapshot.items`, and `subActivity`, and that `mode_ ==` sits between the local take and its give. The submit span asserts `snapshot.items` then `updateRequired_ = false` then `setMask` then give then `M4RenderGuard` then `render()`, with `childScreenOwned_` after the guard. The `loop()` span asserts `if (subActivity)`, then `return;`, then `submitDirtyFrame()`, then `mappedInput`. Plugin enter order, paging, and Wifi transfer asserts stayed. Both printed:

```
app drawer handoff contracts: ALL PASS
PLUGIN_ENTRY_PAINT_CONTRACT_OK
```

### Residual

AppInstall's 60 s abandoned `InstallJob`, and the Registry / Journal serialize and sole-tmp gaps, were not touched. BatchInstall `onExit` still blocks until its worker publishes `done`. There is still no process-wide install mutex. UI-thread `if (mode_ ==` reads are still unlocked against the worker's `mode_ = 0` publish.

No firmware image and no commit. The audit is not finished.

## AppList list ownership (2026-09-27)

Same worktree `/private/tmp/m4-stability-astra-20260926`, same HEAD `2e75e2b140a642a4095f494ae96cb7313f1b6e9d`. No `pio`, no QEMU, no device, no commit. Home was not edited. AppInstall, Registry, and Journal were not edited. BatchInstall `onExit` still waits for `job_->done`. The draw/footer split in the previous section stays. This pass only stops the worker from replacing the vectors and selection the UI reads without the local mutex.

### Pending handoff

`reload` (lines 386–482) still does directory and icon I/O on the worker, under `reloadLock_` then `renderingMutex_`. It does not assign `apps_`, `items_`, `selectedIndex_`, or `mode_`, does not set `updateRequired_`, and does not call `M4ReturnCache::rememberDrawer`. After the in-lock exit check, a dialog (`preserveDialog && mode_ != 0`) stores `verifyDrawerCache_` and returns false. An unchanged drawer returns false. A changed drawer move-assigns into `pendingApps_` and `pendingItems_` and release-stores `pendingReady_` (lines 477–479).

`acceptPendingDrawer` (lines 552–603) is the only writer of those UI fields for a reload result. `loop()` (lines 793–796) calls it after the `if (subActivity)` block returns and before `submitDirtyFrame()`, and only when `exitDisplayTask_` is clear. Exit discards and returns before taking `renderingMutex_`. A live `subActivity`, `childScreenOwned_`, or `mode_ != 0` returns and leaves the pending vectors in place. Otherwise it takes the local mutex for 100 ms, moves pending into `apps_` and `items_`, clears pending, clamps `selectedIndex_` into the new size, and sets `updateRequired_`. `rememberDrawer` runs only after that apply, after the mutex is released, and only if exit is still clear. The worker never calls it, so a result that finishes after `onExit` cannot replace the drawer cache the next page uses.

`discardPendingDrawer` (lines 540–549) takes the local mutex for 100 ms, clears both pending vectors, and stores `pendingReady_` false. The worker calls it before `displayTaskHandle_ = nullptr` and the exit release store (lines 244–248). `onExit` (lines 514–519) still only signals. The destructor clears any leftover pending after `displayTaskExited_` acquire and only then deletes the two mutexes (lines 525–536). `readyForDestruction` still waits for that publication.

`displayTaskLoop` (lines 239–284) skips a new scan while `pendingReady_` is set, including the unchanged path that clears `verifyDrawerCache_`. An unconsumed pending therefore does not start another full scan. The main-thread cache-miss `onEnter` and `uninstallSelected` call `reload()` and then `acceptPendingDrawer()` on the UI thread. The child-return cache miss still only calls `reload()`; the next `loop()` accepts before submit. No lock is held across `loop()` or a callback.

`mode_` is still a plain `int`. The worker no longer stores it. Its `mode_ == 0` reads stay between `xSemaphoreTake(renderingMutex_)` and that take's give. UI writes of `mode_` stay on the UI thread. Unlocked UI reads of `apps_`, `items_`, `selectedIndex_`, and `mode_` no longer race a worker write of those objects.

### Host regression

`cd /private/tmp/m4-stability-astra-20260926 && python3 firmware/tests/test_m4_app_list_lifecycle.py`

ASan and UBSan, exit 0. macOS ASan does not include LeakSanitizer; `sem_live=0` is the leak check. The display-hold block is the pending move-assign while both mutexes are held (`locks=2`). The cross-page block is still inside `M4xRegistry::load` (`locks=1`).

Fresh stdout:

```
display-hold: on_exit_ms=0 ready=0 foreign_deletes=0 sem_deletes=0 locks=2
display-release: ready=1 sem_deletes=2 self_deletes=1
cross-page: on_exit_ms=0 ready=0 foreign_deletes=0 sem_deletes=0 locks=1
cross-page-resume: renders=1 footer=2 guards=1 home_draws=1
cross-page-release: ready=1 sem_deletes=2 self_deletes=1
never-enter: ready=1 sem_deletes=0 sem_live=0
mutex-both-fail: ready=1 reloads=0 tasks=0 sem_deletes=0 sem_live=0
mutex-first-fail: ready=1 reloads=0 tasks=0 sem_deletes=1 sem_live=0
mutex-second-fail: ready=1 reloads=0 tasks=0 sem_deletes=1 sem_live=0
task-fail: ready=1 reloads=1 tasks=1 sem_deletes=2 self_deletes=0
publish-uaf: on_exit_ms=0 deleted_in_printf=1 self_deletes=1 sem_deletes=2 foreign_deletes=0
dirty-frame: copy_assigns=2 copy_ctors=0 items=4 apps=2
frames: initial=1 selection=2 cache=3 apps=1 id=changed
pending-handoff: old_items=4 old_apps=2 accepted_items=7 selected=6 copy_delta=2 remember=1 ensures=1
pending-discard: on_exit_ms=0 renders=1 footer=2 guards=1 remember=0 home_draws=1
pending-discard-release: sem_deletes=2 sem_live=0 self_deletes=1
child-skip: during_child=0 after_child=1
actual app list lifecycle: PASS
actual app list lifecycle: PASS
```

While the worker was blocked inside the pending move, the UI list stayed at 4 items / 2 apps (`plugin-0`, `item-3`) and `selectedIndex_` stayed 3. After the move was released, those UI fields were still the old list until `acceptPendingDrawer`. Dialog, `childScreenOwned_`, and `subActivity` each left pending in place. With `selectedIndex_` at 100, accept clamped it to 6 on a 7-item list (6 builtins plus the reloaded plugin) and called `rememberDrawer` once. That accept added no `TrackedVector` copy-assign and no copy-ctor. The following `submitDirtyFrame` copy-assigned items and apps once each (`copy_delta=2`). A second submit did not render. One unconsumed pending produced `ensures=1` across a 40 ms wait. `onExit` with a pending result returned in 0 ms, rendered nothing, wrote no footer, took no submit guard, and did not remember. After the worker published exit, both pending vectors were empty. Destruction deleted both mutexes (`sem_deletes=2`, `sem_live=0`, `self_deletes=1`). The earlier dirty-frame sample is still `copy_assigns=2`, `copy_ctors=0`.

`firmware/tests/native_app/test_app_drawer_handoff.cpp` and `firmware/tests/native_app/test_m4_plugin_entry_paint.cpp` were updated to this handoff and recompiled. The worker span asserts it does not assign `apps_`, `items_`, `selectedIndex_`, or `mode_`, does not call `rememberDrawer` or `updateRequired_`, and calls `discardPendingDrawer()` before the exit store. The `loop()` span asserts `if (subActivity)`, then `return;`, then `acceptPendingDrawer()`, then `submitDirtyFrame()`, then `mappedInput`. The reload span asserts the pending move and the absence of a direct UI-vector assign and of `rememberDrawer`. Plugin enter order, paging, and Wifi transfer asserts stayed. Both printed:

```
app drawer handoff contracts: ALL PASS
PLUGIN_ENTRY_PAINT_CONTRACT_OK
```

### Residual

AppInstall's 60 s abandoned `InstallJob`, and the Registry / Journal serialize and sole-tmp gaps, were not touched. BatchInstall `onExit` still blocks until its worker publishes `done`. There is still no process-wide install mutex.

No firmware image and no commit. The audit is not finished.
