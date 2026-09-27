# SD 并发证据 — 共享卷缓存窗口（2026-09-27）

工作树：`/private/tmp/m4-stability-astra-20260926`。记录时分支 `feature/m4-stability-astra-20260926`，起点 HEAD `eaaa300b6403e563b1e71b27b75cb8658d0af88f`（其前为 `c65e939`）。本轮只加证据和 host 测试，不改生产锁。

证据分两类。**源码**是本树里读到的路径和行号。**推演**是从这些行推出的交错，固件里没有跑过。**实测**只指下面的 host 程序：它链接 vendored `FsCache.cpp`，在内存块设备上按 `FatFile` 的 prepare→memcpy 窗口做确定性交错。它没有挂载 FAT，没有跑 FreeRTOS，也没有碰物理 SD 或 DMA。

SdFat 来自 `firmware/.pio/libdeps/murphy_m4/SdFat`，`library.properties` 版本 2.3.1。该目录是依赖缓存，不是本分支跟踪的源码。

## 调用链（源码）

`SDCardManager` 没有卷互斥。`open` 直接返回裸 `FsFile`：

- `firmware/open-m4-sdk/libs/hardware/SDCardManager/include/SDCardManager.h:70-75`
- `firmware/open-m4-sdk/libs/hardware/SDCardManager/src/SDCardManager.cpp:389-417`（`openFileForRead` / `openFileForWrite` 同样是 `vol().open`，返回后不持锁）

全文件检索 `SDCardManager.cpp` 没有 `xSemaphore` 或其它 mutex。块设备锁只在扇区拷贝期间持有，拷贝结束就放开：

- `firmware/open-m4-sdk/libs/hardware/SDCardManager/src/SdmmcBlockDevice.cpp:174-177` 创建 `_ioMutex`，注释写明只串行块传输
- 同文件 `214-230`：`readSectors` 在 `xSemaphoreTake` 之后 `sdmmc_read_sectors`，再 `memcpy` 到调用者缓冲，然后 `xSemaphoreGive`
- 同文件 `234-250`：`writeSectors` 同样在持锁时把源拷进 bounce，写完即放锁

因此 DMA 锁的生存期止于 `readSectors`/`writeSectors` 返回。它不覆盖 SdFat 之后对卷缓存指针的使用。

共享缓存（依赖缓存，源码）：

- `FatLib/FatPartition.h:195-223`：一个 `FsCache m_cache`，`dataCachePrepare` 转到 `m_cache.prepare`
- `common/FsCache.cpp:30-50`：`prepare` 在扇区不同时 `sync` 再 `readSector` 进唯一的 `m_buffer`，然后返回 `m_buffer`
- `common/FsCache.cpp:56-69`：dirty 时 `sync` 把这块 buffer 写回 `m_sector`
- `FatLib/FatFile.cpp:828-846`：部分扇区读先 `dataCachePrepare`，再 `memcpy(dst, pc + offset, n)`。`pc` 在 `prepare` 返回之后仍被使用，此时块锁已经放开
- `FatLib/FatFile.cpp:1434-1457`：部分扇区写同样是 `prepare` 返回指针之后再 `memcpy` 进缓存

上层入口都不拿卷锁：

| 调用者 | 位置 | 实际拿的锁 | 与卷的关系 |
| --- | --- | --- | --- |
| `FontManager::scanFonts` | `firmware/src/managers/FontManager.cpp:312-370` | 无。`SdMan.openFileForRead` 后 `openNext`，每 32 项 `delay(1)`，目录句柄仍开着 | 句柄不是卷锁。`delay` 只让出 CPU |
| `scanFiles` | `firmware/src/network/M4FileTransferHttpRoutes.cpp:216-241` | 函数自身无锁。调用点 `handleFileListData` 在 `293-308` 持 `storageMutex_`，循环里 `yield()`（237） | `storageMutex_` 只在传输服务内部。扫描期间会让出，且不覆盖字体扫描或安装 worker |
| `AppInstallActivity::doInstall` | `firmware/src/activities/apps/AppInstallActivity.cpp:101-120`，worker `42-48` | 任务创建后 UI 可离开。worker 调 `M4xInstaller::install` | 不拿 `storageMutex_` |
| `M4xInstaller::install` | `firmware/src/apps/M4xInstaller.cpp:60-71,705-712` | `installGate`（`xSemaphoreTake` 到析构） | 只排除另一个安装。`writeFileBytes`（79-96）和 `extractEntryToFile`（231-238）、`copyListedFiles`（346-355）在 gate 内多次 `openFileForWrite/Read`，中间有 `vTaskDelay(1)`（93）。gate 不覆盖 `FontManager` 或文件列表 |
| HTTP 上传后安装 | `M4FileTransferHttpRoutes.cpp:392-393` 起持 `storageMutex_`，`489-492` 在同一把锁里调 `M4xInstaller::install` | 先 `storageMutex_`，再 `installGate` | 传输路径彼此串行，与字体扫描、AppInstall worker 不串行 |

## 锁顺序（源码 + 推演）

已存在的顺序：

1. HTTP 上传：`storageMutex_` → `installGate` → 多次无锁的 `vol().open` / `FsFile` 读写 → 每次块 I/O 短暂 `_ioMutex`。
2. AppInstall worker：仅 `installGate` → 同样的无锁卷调用 → `_ioMutex`。
3. 字体扫描、以及任何不经过传输 mutex 的 `SdMan.open`：无服务锁 → 无卷锁 → `_ioMutex`。

推演中的交错（本轮未在设备上跑）：T1 在 `FatFile.cpp:836` 拿到扇区 S 的缓存指针后被 `delay`/`yield`/`vTaskDelay` 切走；T2 对扇区 T 再 `prepare`，同一 `m_buffer` 换成 T；T1 回到 `memcpy`，把 T 当成 S。写路径上，T1 的 `memcpy` 会把字节写进已经属于 T 的缓存，随后 `sync` 写到 T。不同 `FsFile` 仍共用这一块缓存，所以把 `_ioMutex` 串行掉也不能关上这个窗口。只删掉 `delay`/`yield` 只是减少切出机会，窗口还在。

只锁 `SDCardManager::open` 也不够：`open` 返回之后的 `read`/`write`/`openNext`/`getName` 仍会 `prepare`。

## Host 实测范围

命令：`python3 firmware/tests/test_m4_sd_cache_interleave.py`

程序 `firmware/tests/sd_cache_interleave_main.cpp` 编译并链接 `FsCache.cpp`。内存设备在 `readSector`/`writeSector` 里用计数锁包住 512 字节拷贝，模拟 DMA 锁的生存期。两个逻辑任务用函数调用交错，不用线程：

- 不加卷锁：T1 `prepare(S)` 之后、`memcpy` 之前跑 T2 `prepare(T)`。T1 读到 T 的字节 `0x5C`，不是 S 的 `0xA1`。设备锁最大深度为 1（块传输仍串行）。
- 同一窗口加卷锁（覆盖 `prepare` 到 `memcpy` 结束）：T2 进不去，T1 读到 `0xA1`。
- 写窗口不加卷锁：T1 `prepare(S, CACHE_FOR_WRITE)` 后 T2 `prepare(T, CACHE_FOR_WRITE)`（dirty 留在 T 上），T1 再写入 `0x3D`。`sync` 后扇区 T 变成 `0x3D`，扇区 S 仍是 `0xA1`。若 T2 是只读 `prepare`，`FsCache::prepare` 会把 dirty 清掉，T1 的 `sync` 变成空操作，写丢失；本测试覆盖的是两次写。
- 同一写窗口持卷锁：只有扇区 S 变成 `0x3D`，T 保持 `0x5C`。

这证明共享 `FsCache` 指针在块锁放开之后仍然有效，并且会被另一次 `prepare` 替换。它不证明 FAT 目录项损坏、不证明物理卡上的 DMA，也不证明字体扫描和安装在设备上已经撞上这个窗口。

## 最小修复边界

要盖住的是每一次会碰共享 `FsCache` 指针的 SdFat 函数整段，从 `prepare` 到该函数返回（含 `memcpy` 和同函数里的 `sync`）。只锁 `SDCardManager::open`、只锁 DMA，或删掉 `delay`/`yield`，都留着这个窗口。

临界区停在该 SdFat 函数返回。字体扫描的 `delay(1)`、HTTP 的 `yield()`、安装里的 `vTaskDelay(1)` 都在 `FsFile` 调用之外，不会持着这把锁。不要把卷锁扩到整次安装、网络收包或 UI。

顺序：调用者可能已经持有 `storageMutex_` 或 `installGate`，然后进入 SdFat，卷锁在其中，`_ioMutex` 只在 `readSector`/`writeSector` 里再套一下。SdFat 不回头等传输锁、安装门闩、渲染或 UI。同一任务可重入（`read` 里再进 `fatGet`）。析构在同一任务释放，错误返回和 `goto fail` 都要等到函数退出才放锁，避免半截缓存操作。

## 实现

`firmware/include/M4SdVolumeGuard.h` 与 `firmware/src/sd/M4SdVolumeLock.cpp`：FreeRTOS 静态递归互斥（`xSemaphoreCreateRecursiveMutexStatic`）。`m4SdVolumeLockPrepare()` 在 `SDCardManager::begin` 里、块设备 `begin` 之前调用；创建被拒绝时返回失败，不空转。Guard 没拿到锁就不增加深度、析构也不释放。Host 测试用同一套接口的 `std::recursive_mutex` 实现（`-DM4_SD_VOLUME_LOCK_HOST=1`）。

`firmware/scripts/patch_sdfat_volume_guard.py` 在 `firmware/.pio/libdeps/*/SdFat` 里插入 guard。除直接的 `dataCachePrepare` / `fatCachePrepare` / `bitmapCachePrepare` / `cacheSync` / `cacheSafeRead` / `cacheSafeWrite` 外，还覆盖拿着缓存指针继续读或写的调用：`cacheDir(`、`readDirCache(`、`dirCache(`、`cacheDirEntry(`、`cacheAddress(`，以及 `FatName.cpp` / `ExFatName.cpp`。锁从函数入口持有到返回，所以 helper 把指针交出来之后，目录项读写和 `cacheDirty` 仍在同一把锁里。`print_t*` 函数不加整函数锁。已安装的 SdFat 必须是 2.3.x，并且规定的 open/getName/日期函数里能看到 guard，否则脚本失败。libdeps 目录还不存在时仍返回 0，避免把依赖还没解压误判成版本错误。`bootstrap_m4_deps.py` 在 PlatformIO 预脚本里再跑一次。libdeps 不进 Git。`FatDbg.cpp` / `ExFatDbg.cpp` 会边打印边走缓存，不在这条产品读写路径上，没有加锁。

没有改 DMA bounce，也没有改字体扫描或安装里的 `delay`。裸 `FsFile` 调用不用各自加锁：窗口在库函数内部，guard 包住整段函数。

## 仍未覆盖

- SdFat 调试转储（`FatDbg.cpp`、`ExFatDbg.cpp`）。
- 依赖缓存被重新解压之后、预脚本还没跑时的中间树。正常 `pio run` 会先打补丁。
- 真机与真实 SD。本轮验收只到隔离 FAT 镜像上的 QEMU smoke。

## 实际验收

起点 HEAD `bdef9abc60deec6696495a768089f6a2edf61d96`。下面是卷锁实现之后的测量。文首“不改生产锁”只描述该起点之前的证据轮。

| 命令 | 结果 |
| --- | --- |
| `python3 firmware/tests/test_m4_sd_cache_interleave.py` | exit 0。输出 `sd_cache_interleave ok dma_copies=12`。链接生产 `M4SdVolumeLock.cpp` 与 vendored `FsCache.cpp`。无锁对照仍把写载荷放到错扇区。生产 guard 持有期间对手线程 `m4SdVolumeLockTry` 失败；释放后深度为 0。`prepare` 失败和提前返回后深度也是 0。 |
| `pio run -e murphy_m4`（`~/.platformio/penv/bin/pio`） | exit 0，SUCCESS。Flash 5,781,873 / 7,143,424（80.9%），RAM 108,924 / 327,680（33.2%）。未烧录。 |
| `pio run -e murphy_m4_qemu_plugin` | exit 0，SUCCESS 80.71s。Flash 5,791,477 / 7,143,424（81.1%），RAM 108,236 / 327,680（33.0%）。`firmware.bin` 5,791,825 字节。未烧录。 |
| `M4SIM_TMP=/tmp/m4-sd-guard-smoke-20260927 ./m4sim run --plugin-debug --skip-build --no-hostfwd --no-net --fresh-sd --ready-seconds 45` | exit 0。新 64 MiB FAT：`/tmp/m4-sd-guard-smoke-20260927/artifacts/murphy-sd.img`。QEMU PTY `/dev/ttys235`。第一次 ping 在启动早期超时，第二次 0.6s 就绪：`activity=Home`，`sd_ok=true`，`firmware=202608187-murphy-m4-qemu-plugin`。随后同一 `M4SIM_TMP` 下 `./m4sim stop`。未用 `/dev/cu.usbmodem101`，未写用户卡。 |

Host 程序没有链接 `FatFile.cpp`，也没有 FreeRTOS 抢占。QEMU smoke 只证明这版固件能挂上隔离 FAT 并回到 Home，不证明两个任务已经在设备上交错过缓存。

## 仍存漏洞

- `FatDbg.cpp` / `ExFatDbg.cpp` 未加 guard。产品读写不走这两份调试转储。
- 新解开的 SdFat 在 `pio run` 预脚本跑完之前没有 guard。
- 未测真机、未测真实 SD、未测字体扫描与安装在设备上的碰撞。
- 卷锁不覆盖 `delay`/`yield` 期间的目录句柄；那些让出点本来就不持卷锁。目录项一致性不在本轮范围内。

## 阶段 handoff

Grok 4.7 zxl4869 在原分支提交卷级 guard、补丁脚本、host 回归和本文。libdeps、固件 bin、测试镜像不进 Git。未 push。下一阶段用提交 SHA 验收。真机仍未测。

