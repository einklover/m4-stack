# M4 Astra stability audit — 2026-09-26

Independent review on `feature/m4-stability-astra-20260926`, starting from Sol `971be6d23586e87b8183a52275448bcac25fdf21` (itself on `feature/firstboot-home-appstore-20260926` / `846b1fa`). Line numbers below are from this branch after the Astra edits. Grok / Luna / Muse reports were inputs, not orders.

Worktree: `/private/tmp/m4-stability-astra-20260926`. No Paseo edits, no real-device flash, no user SD.

## What was already true before this pass

| Claim | Verdict |
| --- | --- |
| Grok: `removeDir` infinite recursion on `.` / `..` | False for pinned SdFat 2.3.1. `FatFile::openNext` skips those FAT entries. Sol still caps depth at 16 and closes handles (`SDCardManager.cpp` line 450). |
| Muse: TTF switch always leaves a dangling NOTOSANS alias | Not the current M4 runtime TTF path. That path binds a hash id. Reader handoff and quiescence contracts still pass. Concurrent font use on device is not proven safe by that. |
| Luna: Clear Cache deletes reader progress | True, and already fixed on Sol. `M4CacheClearPolicy.h` lines 6–9 keep `progress.bin`, `progress.dat`, and `progress.tmp`. |
| Per-transfer DMA bounce malloc | True, and already fixed on Sol. One 1024-byte internal DMA buffer plus an I/O mutex, two sectors at a time (`SdmmcBlockDevice.cpp` lines 176–241). |
| Settings `O_TRUNC` in place | True, and already fixed on Sol (temp, sync, backup, rename, file mutex). |
| Early SD failure had no user retry | True, and already fixed on Sol. Confirm retry is only before settings and other file owners (`main.cpp` lines 958–1008). A later I/O failure does not remount. |

## New findings, with the fix

### 1. Worker tasks leaked C++ objects and could stay busy forever

`M4Psram::deleteTask(nullptr)` does not unwind C++ frames. Catalog, Discovery, Login, and book-detail task entries returned from inside the business function on several paths, including the Legado cache hit at `M4NativeProviderCatalog.cpp` lines 814–827. That skipped both destructors and `gBusy = false`, so the next catalog start failed the compare-exchange and the UI waited.

App Store `taskEntry` deleted the task on the catalog branch while `unique_ptr<Job>`, strings, and the app vector were still alive (`AppStoreActivity.cpp` previously returned before the shared cleanup).

Fix: each provider `runJob()` returns through normal C++ scope. `taskMain` then clears `gTask` under the mutex, then stores `gBusy` false, then self-deletes (Catalog lines 880–888; same shape in Discovery, Login, and BookDetail). App Store runs the work in a lambda, resets the job and the shared state, then self-deletes (`AppStoreActivity.cpp` lines 159–286). Cache-hit and other early paths already `publish` a terminal phase before returning, so clearing busy does not leave the UI on Connecting.

Host contract: 1000 iterations × 4 providers, 4000 early returns, live objects 0.

### 2. Cutting a directory could delete the source and leave an empty destination

The paste path created the destination directory and then recursively deleted the source. A failed or partial copy destroyed the user's folder. File copy also treated a negative SdFat read as a huge `size_t` and ignored short writes, and it left the source handle open when exclusive create failed.

Fix: cut is `SdMan.rename` for files and directories, and it refuses to move a directory into itself (`MyLibraryActivity.cpp` lines 1239–1256). Copy is files only; a directory copy tells the user to use cut. `copyFile` (lines 81–109) uses `O_WRONLY | O_CREAT | O_EXCL` and `M4CheckedCopy` (1024-byte stack buffer, signed read, full write, `sync`). Failure deletes only that new partial file. The source is closed on every exit.

### 3. Unknown-length HTTP could ignore the idle timeout

`HTTPClient::writeToStream` can loop while `available()` stays 0 and never call the sink. The previous timeout wrapper only ran on a write, so a silent socket never tripped it. File download loops already call `M4HttpDownloadPolicy::timedOut` and must not be forced through `connected()==0`, or an unknown-length body can be reported as success.

Fix: `M4DeadlineClient` (`firmware/src/network/M4HttpDownloadPolicy.h` consumer, header `M4DeadlineClient.h`) checks idle and total time on `available`, `connected`, and `read`. `startBody` is called only from `fetchUrlBounded` (`HttpDownloader.cpp` line 114). File downloads construct the same client type but do not arm it; their own loops keep the 128 MiB / 15 s idle / 10 min limits. Chunked decoding stays inside `HTTPClient`.

### 4. A short settings write could still be committed

Sol's commit protocol is kept. `measureJson` and `serializeJson` in ArduinoJson v7 share `JsonSerializer`. A mismatch, overflow, write error, or failed `sync` now deletes the temp file and leaves the previous primary or backup (`CrossPointSettings.cpp` lines 272–280). This fails closed. It does not rewrite a good file with a short one.

### 5. The library listed and sorted the whole directory on the UI task

`loadFiles` used to read every accepted name, then sort the full vector, on the 16 KiB loop task. A directory of thousands of books blocked the UI for the whole scan and held name storage proportional to the directory.

Fix (`MyLibraryActivity.cpp` `startDirectoryPage` line 276, `scanDirectoryBatch` line 304, `finishDirectoryPage` line 385):

- Each UI turn opens the directory, reads at most 16 entries or 4 ms, then closes the handle before returning.
- A page keeps at most 64 accepted names or 8 KiB of name bytes. Within the page, directories sort before files. Global alphabetical order of a huge folder is intentionally gone; every accepted file stays reachable only if the cursor below is right.
- The cursor is the next 32-byte SdFat slot, not a dense index. The slot of an entry that does not fit is saved before it is consumed. An unaligned cursor, a seek/`openNext` I/O error, or a partial batch whose `curPosition` is not 32-byte aligned (lines 373–382) clears the page and shows “目录读取失败，请返回重试”. Keeping the previous aligned cursor in that last case would reread the same slots on every UI turn. The error page does not insert a “回到首批” row.
- `getName` returning 0 (empty or longer than the 768-byte buffer) skips that entry. It does not fail the directory.
- “回到首批” / “下一批” are sentinel rows added after the sort. Touch, confirm, and the action menu consume them before opening a path.
- Up to 8 crumbs remember the parent page and the entry name. Back during a load returns to that parent (Home at the card root) instead of always leaving the activity.
- One log line per finished page includes stack high-water. No per-entry log.

Host exercise: 0, 64, 65, 1000, and 10000 short names, 10000 long names, 10000 hidden files, an I/O fault, a 900-byte rejected name beside `ok.txt`, pending selection of `07.txt`, a seek to slot 1, and a partial batch that ends one byte off a slot. Each non-zero size is seen once. Reads stay within `count + pages + 1`. The directory handle is closed between turns, including while a page is unfinished. The off-slot batch ends with an empty error page.

### 6. Search yielded while the child handle was still open

Recursive search already had Sol's budget (8 levels, 4096 entries, 512 hits, 3 s) in `M4LibraryScanPolicy.h`. It called `delay(1)` before `getName` and `close`, so two handles were live across the yield. A failed `getName` also leaves an empty string; using it would walk a blank path.

Fix (`MyLibraryActivity.cpp` lines 123–131): read the name and size, close the child, then yield. Skip empty names, dotfiles, and `System Volume Information`. The parent directory handle is still held across that yield and across the recursive call. That is bounded by the depth cap. It is not a global SdFat lock.

## Explicitly not changed

- No process-wide SdFat mutex. Display and network locks are taken on other tasks; a volume lock grabbed from those paths can invert. Closing the library handle between batches is the boundary this pass can prove.
- No runtime remount. Confirm retry remains the early-boot window only (`main.cpp` 958–1008). After `capabilityProbe`, setup continues into `SETTINGS.loadFromFile()` (line 1032). A failed probe still stops at `SD: io_failure` and does not format the card.
- TTF faces, EPUB/TXT prefetch, image decode, and Lua lifetimes were not given a new allocator. Home already logs internal, DMA, PSRAM free/largest and main-task stack high-water (`main.cpp` lines 1761–1762). Those numbers on QEMU are not device heap.
- `/FONT` scan stays capped at 512 entries per directory and still holds its directory handle across `delay(1)` (`FontManager.cpp` lines 327–330).
- DMA and TLS buffers stay in internal RAM. PSRAM is not used for them.

## Historical items that remain open

- Cross-component SdFat metadata races outside the library pager. Search and font scan still hold a directory handle across a 1 ms yield.
- Runtime card removal after a successful mount. Remounting while any handle exists can corrupt the volume. This build does not do that.
- Long-run fragmentation of TTF, reader prefetch, and image decode. Needs a device loop. QEMU heap is not that measurement.
- A huge folder is no longer one sorted list. Order is per page. A wrong 32-byte resume would skip or repeat names; it does not write the card. Device LFN resume was not executed here.
- Loop-task stack high-water under a real TTF library paint. The display task was already gone on this branch; this pass renders the library from `loop()` and logs high-water. It does not restore a second display task.
- `test_m4_font_loader_contract.py` has a pre-existing string assertion failure on font code this pass did not change. `test_settings_theme_minimal.py` needs pytest, which this environment does not have in the default `python3`.

## Verification

Host, on this tree, after the final source edits:

- `python3 firmware/tests/test_m4_astra_stability.py` — task wrappers PASS (1000×4 providers, 4000 early returns, live objects 0); directory scanner PASS, including 1k/10k/10k long names, I/O faults, and the off-slot partial batch.
- `python3 firmware/tests/test_m4_dependency_bootstrap_contract.py` — PASS.
- `python3 firmware/tests/test_m4_wifi_transfer_phase1_contract.py` — PASS.
- `python3 firmware/tests/test_reader_ttf_quiescence_contract.py` — PASS.
- `python3 firmware/tests/test_reader_font_reload_handoff_contract.py` — PASS.
- `python3 firmware/tests/test_reader_persistence_lock_contract.py` — PASS.
- Native asserts, exit 0: `test_m4_checked_copy.cpp`, `test_m4_deadline_client.cpp` (ASan/UBSan), `test_m4_cache_clear_policy.cpp`, `test_m4_http_download_policy.cpp`, `test_m4_library_scan_policy.cpp`, `test_m4_settings_commit.cpp`.

Production `pio run -e murphy_m4` SUCCESS. Flash **5,777,313 / 7,143,424 bytes (80.9%)**, inside the APP1 slot at `0x6e0000` (`partitions_murphy_m4.csv`, size `0x6d0000`). Object file for `MyLibraryActivity.cpp` is newer than the source that contains the off-slot check. Local preview, not committed:

- Path: `firmware/.pio/build/murphy_m4/firmware.bin`
- SHA-256: `7e7c1bd0587fd4eb66f966f10508c594e32bf783c24674902139629207bd2ffe`

`pio run -e murphy_m4_qemu_plugin` SUCCESS. Flash 5,786,877 / 7,143,424 bytes (81.0%) for that debug env. Isolated smoke used a new 64 MiB FAT image at `/tmp/m4-astra-smoke-20260926/artifacts/murphy-sd.img` and QEMU PTY `/dev/ttys235` (`m4adb --port` / `--no-daemon`). The first ping timed out during early boot; the next ping was ready in 0.6 s with `activity=Home` and `sd_ok=true`. Result: `SMOKE PASS`. The session was stopped. That image was not the user's card, and the port was not `/dev/cu.usbmodem101`.

A green link and a QEMU Home screen are not a hardware pass. QEMU does not show SD electrical retry, device heap, device loop-stack high-water, or long-run fragmentation. The QEMU `free_heap` / `free_psram` fields are simulator status only.
