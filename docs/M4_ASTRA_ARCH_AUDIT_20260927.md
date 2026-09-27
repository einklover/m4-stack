# M4 修复后架构审计 — 2026-09-27

供主 Grok 实施。基线：`/private/tmp/m4-stability-astra-20260926`，分支 `feature/m4-stability-astra-20260926`，HEAD `4c9739aeceb0266c3651eecff7f2701bced1f501`。审计开始及结束前已执行 git status / rev-parse，均 clean、SHA 一致。重点审阅 e0c9388、5cc999e 的修复及相关调用链。

**证据边界：以下全部为静态代码审计，未做本轮 host 故障注入或设备复现。没有运行 PIO/QEMU，没有接触真实 SD、修改固件/测试/仓库文件、提交或修改 Paseo。** 路径相对上述 worktree，行号针对基线。SdFat 底层补充证据来自该 worktree 现存依赖缓存（library.properties 声明 2.3.1），不等同于 Git 跟踪文件。

## 1. P1：SdFat 共享缓存竞争，DMA 锁不能保护卷操作

**分类：已知 backlog，新增底层竞争证据。**

源码：
- `firmware/open-m4-sdk/libs/hardware/SDCardManager/include/SDCardManager.h:70`，`src/SDCardManager.cpp:389-412`：直接返回裸 FsFile，没有统一 volume 互斥。
- 同目录 `src/SdmmcBlockDevice.cpp:174-177,214-250`：1024-byte DMA bounce 的 mutex 仅保护块传输。
- `.pio/libdeps/murphy_m4/SdFat/src/FatLib/FatPartition.h:195-223`：共享卷缓存；`common/FsCache.cpp:30-50,56-69`：共享 buffer/sector/status 的修改与回写。
- 同依赖 `FatLib/FatFile.cpp:835-846,1451-1457`：prepare 返回缓存指针后，调用者才 memcpy，底层块锁已释放。
- `firmware/src/managers/FontManager.cpp:328-330,369-371`：扫描持目录句柄 delay；`firmware/src/network/M4FileTransferHttpRoutes.cpp:216-241`：持 root 句柄 yield。

**可达触发链：** AppInstall worker 离开界面后仍安装，同时 Home backend 读 registry/封面，或 Reader 加载字体。T1 prepare 扇区 S 后、memcpy 前被切走；T2 prepare 扇区 T，覆盖同一缓存；T1 恢复后读错字节或把 S 的内容写进 T 的缓存。不同 FsFile 仍共用卷缓存，单次 DMA 串行无效。最终是否损坏 FAT 取决于具体交错，**未实测损坏**。

注意：font scan 本身持的是句柄，不是 volume 锁；仅“持句柄 yield”不足以证明损坏，根因是无互斥的共享缓存操作。

**最小修复：** 保护完整 SdFat 操作及内部缓存指针使用期，覆盖裸 FsFile 调用；只锁 open 或 DMA 不够。未覆盖入口应禁止并发。临界区不能跨网络等待、整次安装或 worker join。顺序约束：服务/安装事务/registry → volume → device；持 volume 禁止反向等待 render/font/UI。

**最小验证：** 内存块设备上，确定性暂停在 prepare→memcpy 窗口，两任务读写不同扇区，验证数据和目录一致；再组合目录扫描与安装。host 不证明物理 DMA/SD 行为。

## 2. P1：恢复流程绕过 install gate，误清正在安装的目录和记录

**分类：交接已知 backlog，补足可达破坏序列。**

源码：
- `firmware/src/apps/M4xInstaller.cpp:59-69,706-717,835-840`：gate 只覆盖 install/uninstall。
- 同文件 `614-626`：ensureLayout 无条件 recovery，probe 又调用 ensureLayout。
- `firmware/src/activities/apps/AppListActivity.cpp:254-269,386-397`：后台 reload→ensureLayout；HTTP `firmware/src/network/M4FileTransferHttpRoutes.cpp:489-492` 同样先在 gate 外 ensureLayout。
- Installer `745-755`：先写 Staging journal，再提取文件；`497-501` 是删除 staging 的生产 hook。
- `firmware/src/apps/M4xInstallTxn.h:113-117`、`M4xInstallJournal.cpp:359-380,437-445`：Staging 被恢复流程删除，并保存剩余记录。

**可达触发链：** T1 安装发布 Staging、正在 extractListed；用户 Home 离开后进入 AppList，T2 reload→recoverAll，把 T1 的 staging 当断电残留删除并清 journal。即使每个 SD 操作已经串行，此逻辑竞争仍存在。另一个窗口：T2 loadAll 得到空集合，T1 写入新记录，T2 saveAll(empty) 将其覆盖。**未做运行复现。**

**最小修复：** 分离建目录/只读 probe 与破坏性 recovery；install/uninstall/recovery 整体归同一事务 owner。内部用明确 already-locked helper，避免 install→probe 再取非递归 gate。drawer reload 不应隐式做恢复。

**最小验证：** 内存文件系统在 Staging 提取中、目录切换中、recoverAll 空快照保存前设置 barrier，再调用另一个公开入口；不得删除活跃事务目录或覆盖其记录。必须覆盖真实公开入口，不能仅测纯恢复状态机。

## 3. P1：Registry::load 可在 save 成功后把 primary 回退为旧版

**分类：相对已读交接/审计的新发现；独立于第 2 项。**

源码：
- `firmware/src/apps/M4xRegistry.cpp:136-150`：load 读不到合法 primary 就读 bak，并删除当前 primary、回写 bak；没有读写事务锁。
- 同文件 `184-207`：save 写 tmp，primary→bak，然后 tmp→primary，两次 rename 间 primary 正常缺席。
- `firmware/src/activities/home/HomeActivity.cpp:330-333`、`AppListActivity.cpp:397`：普通读取者不持安装 gate。
- `firmware/src/apps/M4xInstaller.cpp:818-826`：成功后确认 journal 并清理备份/记录。

**可达触发链，无需 I/O 错误：**
1. T1 save 已将 OLD primary→bak，暂停。
2. Home T2 load 发现 primary 缺席，读取 OLD bak，暂停在 Registry.cpp:145 前。
3. T1 tmp→primary 成功，返回成功，安装可以清 journal。
4. T2 恢复，删除现在已是 NEW 的 primary，再写回 OLD。

结果可能是安装已报告成功、live 已更新，但登记表版本/文件 inventory 回退。单次 volume 锁与 install-only gate 均不足。**未做运行复现。**

**最小修复：** 优先让普通 load 只读，primary 修复交由独占 recovery；若保留自修复，完整 load 选择/修复与 save 替换共用 registry 事务锁，顺序 install→registry→volume，锁内不反调 installer。

**最小验证：** 内存 SD 严格执行上述四步；save 成功后恢复旧 loader，最终 primary 必须仍是 NEW。另测真正重启后读取 OLD bak。现有单线程 writer 故障注入不能覆盖该窗口。

## 4. P1：WebSocket 上传的 mutex 由 UI 获取、cleanup 任务释放

**分类：相对已读交接/审计的新发现。**

源码和链路：
- `firmware/src/network/M4FileTransferService.cpp:116` 创建 FreeRTOS mutex。
- UI `firmware/src/activities/network/CrossPointWebServerActivity.cpp:388-389` → service `180-191` → `M4FileTransferAuxiliaryServer.cpp:70-73` 的 WS poll → START `172-204` → `acquireStorage:106-111`，UI 持锁跨上传帧。
- Activity `onExit:130-172` 将 context 交给 WebServerCleanup；其 trampoline `44-55` 调 service.stop。
- service `209-216` → auxiliary stop `54-55` → abortUpload `127-141` → releaseStorage `114-117`：由另一任务 give。

**可达触发：** WS START→READY 后保持连接、不发完数据，设备 Back/Home。此时确定违反 mutex 同任务 take/give 的所有权要求。**未实测该镜像是否断言复位或产生何种优先级异常，不提供虚构 panic。**

同一链还有 UI 停顿：HTTP `M4FileTransferHttpRoutes.cpp:392,455-470` 持 storageMutex 收包，每收到数据重置重试计数，无总时限；UI 收到 WS START 后在 auxiliary `109` 无限等锁。service `184-191` 丢弃 budgetMs，abortCheck 需等 poll 返回。慢速持续 HTTP 上传因此可阻塞整个 UI 输入处理；未实测耗时。

**最小修复：** 原 UI owner 在转交 cleanup 前终止 WS upload、关闭 partial 并归还 mutex；后台不代替 owner 解锁。UI acquireStorage 用 try/timed take，busy 返回错误。不要仅换 binary semaphore 掩盖事务所有权。保留 HTTP handler 结束后才释放 routes/mutex 的关闭次序。

**最小验证：** owner-aware semaphore shim，运行 START→READY→Home；每次 give 的 task id 必须匹配 take。再测 HTTP 持锁→WS START→Back，UI 有界返回。禁止使用允许任意任务 give 的宽松 stub。

## 5. P2：JPEG 尾部分配绕过已有 PSRAM 失败返回，低内存可能升级为崩溃

**分类：新的具体失败处理缺口；不是已证长期 OOM/泄漏。**

源码和可达链：
- Home 缺缩略图：`firmware/src/activities/home/HomeActivity.cpp:535-552,387-404` → `firmware/lib/Epub/Epub.cpp:550-586` → `firmware/lib/JpegToBmpConverter/JpegToBmpConverter.cpp:675-677` → internal converter。
- converter `363-381` 对主 scratch 缓冲检查失败；但 `393` 普通 new 创建 ditherer，`411-412` 普通 new[] 创建缩放累计行，没有失败路径。
- `firmware/lib/GfxRenderer/BitmapHelpers.h:18-21` 内部三次 new[]，`41,60-65,75` 直接使用；converter `555-556,614-615` 直接使用累计行。

**触发条件：** 合法缩放 JPEG，前面 scratch 分配成功，后面的任一次 C++ 分配失败。此链不能保证返回 false：标准 throwing new 会抛出该链未捕获的 bad_alloc；也不能假定禁异常运行库会安全返回业务失败。若非标准 new 返回 null，后续亦无检查。

**明确限制：未注入分配失败，未观察设备 OOM、持续泄漏或碎片趋势；普通 new 的实际堆路由也未证实，不能称这些缓冲必然占 internal DMA heap。** 本项确认的是失败处理缺口。

**最小修复：** 局限 converter 与所用 ditherer，复用 allocScratch 或显式可失败分配；包括构造器内部三行在内，每步可检查，统一释放后返回 false。外层单加 nothrow 不足。复用 Epub.cpp:591-593 的失败删除缩略图路径，不新增 allocator/arena，不移动 DMA bounce。

**最小验证：** 有效 JPEG fixture，逐点拒绝 dither 三行及累计两行分配；必须返回 false、无 abort/空指针、已分配资源归零，随后正常图片仍可转换。

## 已确认的审计边界与实施建议

AppList pending 交接/退出 publication、AppInstall timeout 保持 job 所有权，未发现新的直接反证；不重复报告已修复 UAF。TTF cache 有容量/释放路径，TXT 大缓冲走已有 PSRAM 管理；没有本轮长期 OOM 结论。坏 primary/唯一有效 tmp 的恢复问题是已知 backlog，不包装为新发现。

BatchInstall `onExit`（`firmware/src/activities/apps/BatchInstallActivity.cpp:160-169`）仍无限等 worker；AppList UI 同步 uninstall（`AppListActivity.cpp:752-758`）仍可能长时间等 gate/删目录。这些属于已知长任务 backlog，不再扩展列项。

**最高优先级建议：先修第 2、3 项，形成一致的安装/恢复/registry 事务边界，并修第 4 项明确的 mutex owner 错配；第 1 项必须按完整调用覆盖与锁顺序单独收口，不能零散补锁。第 5 项做局部失败处理。**

最省验证成本的两组 host 工作：① 内存文件系统 + 确定性 barrier，复现第 2/3 项并覆盖第 1 项共享缓存窗口；② owner-aware RTOS shim 验证第 4 项，独立 JPEG fixture 分配失败注入验证第 5 项。以上为交给 Grok 的方案，本轮未执行。设备与长期内存验证需后续另行安排，不能用历史编译或 boot ping 代替。
