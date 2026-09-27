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

## 最小修复边界（本轮不改代码）

要盖住的是每一次 SdFat 调用里、缓存指针还活着的整段，包括裸 `FsFile` 的 `read`/`write`/`openNext`/`getName`/`close` 以及 `mkdir`/`remove`/`rename`/`exists`。未走这把锁的入口不能和已加锁的入口并发。

临界区停在单次卷操作结束。不要跨 HTTP 收包、`httpd_resp_send_chunk`、整次 `M4xInstaller::install`，也不要在持卷锁时 `vTaskDelay` 等 worker。`storageMutex_` 今天已经跨了上传和目录列表里的 `yield`，那是另一把锁，不能拿它冒充卷锁，也不该把卷锁嵌进那段网络等待。

顺序保持：服务锁 / `installGate` / registry 写锁 → 卷锁 → `_ioMutex`。持卷锁时不要再等渲染、字体或 UI。HTTP 上传已经是 `storageMutex_` 然后 `installGate`；卷锁只能加在这两者之内的每次卷调用上，不能反过来让持卷锁的人去等 `storageMutex_`。

下一步只做这一层：给上述卷操作加短临界区，并用本 host 测试的“持卷锁则扇区不串”作为回归。不要在这一步改 DMA bounce，也不要先删 `delay`/`yield` 当作修复。
