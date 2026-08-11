## 发布 2026-08-04 (firmware sha256 651f22bd…)

- 固件: 双 UA 头修复 (net.request/dl.jsonGet/jsonToFile), dl 回调墙钟预算扩展,
  QRCodeHelper 自动选版 (V4 超容损坏修复), validJsonShape 允许空 path (正文 file_out).
- 插件:
  - weread.m4x v0.6.8
  - fanqie.m4x v0.2.17
  - jjwxc.m4x v0.1.0 (晋江文学: 分类浏览/目录/免费章/VIP章扫码登录阅读)

# Murphy M4 开发踩坑全量手册

> 汇总自全部历史开发文档（docs/ 34 篇 + task_prompts/ 14 篇 + 实战记录），覆盖硬件、刷写、SD 卡、网络、Lua 沙箱、阅读器、并发、字体、安全等全部主题。接手开发者先读 TOP 15，再按分类查细节。

## 0. 总体原则（贯穿全部）

- **真机现象是最高权威**："physical failures are authoritative"。模拟器全绿 ≠ 没问题，真机验证至少覆盖：扫码/session 恢复、书架、目录触摸、缓存章直开、切章、返回桌面、错误后资源释放。
- **串口单 owner**：同一时间只有一个程序碰串口；反复 open/close 会复位甚至让设备完全静默（只能断电）。
- **只刷 APP1**（`0x6e0000`），绝不写 APP0/bootloader/分区表/NVS；不整片擦除。
- **分区预算**：RAM ≤35%、APP1 ≤85%（硬限 `0x6d0000`）；内存验收分域报告（内部 RAM / PSRAM / Lua 堆），不许加总。
- **插件改源码必须 bump versionCode**，否则 `noop:true` 静默吞掉更新；`.m4x` 包与源码必须 parity。
- 流程：模拟器回归（ctest）→ 编译 → 真机；每个新构建单独走 APP1 授权流程。

---

## 一、硬件 / 串口 / USB

**[H-1] 设备"不断重启"（看起来像死循环重启）**
- 症状：连续执行 `m4adb ping/status/install`（每次新开串口）后设备反复 `rst:0x15 (USB_UART_CHIP_RESET)` 重启；flash 刚结束立刻被另一进程抢串口也会复位。
- 根因：ESP32-S3 USB Serial/JTAG（CDC）在主机侧 `open()/close()` 时经常触发 `USB_UART_CHIP_RESET`；host 串口生命周期在打断 boot，不是固件无故重启。
- 解决/规避：全程唯一串口 owner；优先一条持久 `m4adb shell` 会话跑完 ping→status→install→launch；禁止 `for i in range(N): open→ping→close`；flash 结束后等设备重新枚举 + 5–15s 再连 m4adb。怀疑重启时先 `lsof /dev/cu.usbmodem*` 检查占用，停掉 host 进程后设备稳定即确诊为串口 thrash。（M4_USB_SERIAL_OPS.md、M4_AI_UPDATE_GUIDE.md §2/§7.2、M4_SERIAL_DEBUG_BRIDGE.md）

**[H-2] 设备完全静默（连 esptool 都收不到数据，只能物理断电）**
- 症状：反复 open→超时→close 之后设备无 boot 日志、无桥响应，esptool 报 `No serial data received`（USB 仍枚举）；用户物理断电重启才恢复。
- 根因：多次 open/close 制造 reboot storm，USB JTAG 进入静默状态。
- 解决/规避：任何失败后先 close 并等待 10–20s 再考虑第二次 open；诊断脚本被 kill 会留半开连接，操作前 `lsof` + pkill 残留；设备静默时断电重启，不要盲目重刷。（M4_UPDATE_BLOCKED_SD_WRITE_20260801.md §1.4、M4_DEVELOPMENT_NOTES.md §2）

**[H-3] 刚 open 串口后 ping 必失败**
- 症状：`open` 一次 = 复位一次，boot 到 Home 约 4–6s；`ping` 默认 5s 超时在刚 open 后必然失败。
- 根因：boot 未完成，桥未就绪。
- 解决/规避：用 `client.wait_ready(timeout>=60)` 或先 drain boot 日志（看到 `BOOT_SUMMARY` / `Entering activity: Home` / `USB serial debugging ENABLED`）再操作。（M4_AI_UPDATE_GUIDE.md §7.2、M4_USB_SERIAL_OPS.md §2.5）

**[H-4] 桥无响应/握手超时**
- 症状：`@M4DBG` 无响应、`等待响应超时`，或只能看到启动日志。
- 根因：开发者选项"USB 串口控制"默认**关**；桥关闭时固件丢弃 CDC 输入，且刷机不会替你打开开关；也可能是仍在 boot 或 e-ink 阻塞。
- 解决/规避：设备上 设置→系统→开发者选项→USB 串口控制=开；仍超时则拔插 USB、释放端口；桥开关跨重启持久，不信任的 USB 主机上不要长期开着。（M4_SERIAL_DEBUG_BRIDGE.md、M4_USB_SERIAL_OPS.md §2.6、§5）

**[H-5] m4adb daemon 双开抢端口**
- 症状：命令连不上 socket 自动起新 daemon → 双 daemon 抢端口 → 设备复位；`--no-daemon` 与常驻 daemon 混用 = 双端口冲突。
- 解决/规避：遇到异常先 `pkill -f m4adb.py; rm -f /tmp/m4adb-*.sock` 再重来；最稳模式是单连接脚本（`Client(SerialTransport(port,115200))`）跑完全流程；设备重启会导致 daemon 死、日志流丢失，抓日志时留意。（M4_DEVELOPMENT_NOTES.md §4）

**[H-6] 截图是 PBM，OCR 失败**
- 症状：tesseract 读 /tmp 下文件有时失败。
- 解决/规避：用 PIL 转 PNG 后 `tesseract -l chi_sim` OCR，且放工作目录运行。（M4_DEVELOPMENT_NOTES.md §5）

**[H-7] 长时间持续翻页后宿主/串口偶发冻结**
- 症状：长时间连续翻页后宿主或串口偶发冻结（未完全定位）。
- 根因：疑似 e-ink 连续刷新或宿主 UI 循环（未最终定位）。
- 解决/规避：断电重启即可恢复，不影响已修复功能；已修复功能不受影响。（M4_DEVELOPMENT_NOTES.md §5）

**[H-8] launch 新 app 时旧 Reader sub-activity 残留占屏**
- 症状：tap 落错地方。
- 解决/规避：先 `key("back")` 退出再 launch。（M4_DEVELOPMENT_NOTES.md §5）

**[H-9] 触摸历史不响应 / 前光太暗（RC1 硬件观察）**
- 症状：FT6x36 触摸曾不响应；前光偏暗。
- 根因：触摸需要 power gate OFF→ON + 250ms 后再 3000ms settle；tap 用 touch-down 坐标而非 release；前光 boot 早期 20% / warmth 50% 且 clamp 缺失。
- 解决/规避：`hasTouch` 配置化 + `isTouchStreamReady` 在 settle 与首帧成功后才为真；前光加 `M4FrontlightPolicy`（0=off、clamp 0–100）每次 apply 归一化。（MURPHY_M4_RC1_PHASE3_LOG.md §3/§4/§5）

---

## 二、刷写 / OTA

**[F-1] `pio run -t upload` 不安全**
- 根因：`board_upload.offset_address` 不是安全保证；generic upload 不能保证只写 APP1（双 OTA 设备可能破坏原厂 APP0）。
- 解决/规避：只用 `scripts/murphy_m4_app1_flash.py --i-understand-app1-only`（写 `0x6e0000` + 校验 hash + 切 slot 1）；切回原厂用 `--select-slot 0`（只切槽不擦 APP1），禁止整片擦除或 `pio upload`"恢复原厂"。（MURPHY_M4_APP1_FLASH.md、M4_AI_UPDATE_GUIDE.md §1、M4_DEVELOPMENT_NOTES.md §3）

**[F-2] otatool 自动选槽失败：`No module named esptool`**
- 症状：固件写入成功但 `otatool.py switch_ota_partition --slot 1` 失败；parttool 读分区表失败。
- 根因：otatool 内部用 `sys.executable` 拉起框架自带 esptool.py；本机 `/opt/anaconda3/bin/python` 无 esptool 模块。只设 `IDF_PATH` 不够。
- 解决/规避：用 `~/.platformio/penv/bin/python` 跑同一 otatool 命令即成功；选槽后必须 `read_otadata` 复核（`Firmware: 0x00000010` = slot 1）；指南里 `PYTHONPATH=tool-esptoolpy` 方案未在本机验证。另：因系统 Python 缺 rich_click 导致自动选槽失败时，不要重复写固件，只用 PlatformIO Python 重试槽位切换。（M4_UPDATE_BLOCKED_SD_WRITE_20260801.md §1.1、M4_AI_UPDATE_GUIDE.md §1.2/§7.1）

**[F-3] 全片写工厂镜像：高波特率下 USB 应答丢失**
- 症状：921600 波特全写 16MiB 在接近完成时丢失 USB 响应；460800 重写在 99.9% 处又丢最后一次 USB ack。
- 根因：USB transport/acknowledgement 问题，不是 flash 内容失败——独立 `verify_flash`（115200）比对全部 `0x1000000` 字节 `Verification successful (digest matched)`。
- 解决/规避：写入失败后必须独立 verify 再下结论；esptool 经 RTS 硬复位。（MURPHY_M4_FACTORY_RESTORE_20260728.md）

**[F-4] 开发期分区表与工厂分区表不一致，切槽不安全**
- 症状：开发镜像把分区表改成了 test layout（`app0@0x10000/0x400000`、`assets@0x410000/0x580000`、`app1@0x990000/0x60000`），而工厂布局是 `app0@0x10000/0x6d0000`、`app1@0x6e0000/0x6d0000`。
- 根因：分区表被替换后仅切换 OTA 槽不安全/不充分。
- 解决/规避：该次恢复采用全片回写工厂镜像并逐字节校验；日常开发禁止写分区表。（MURPHY_M4_FACTORY_RESTORE_20260728.md）

**[F-5] APP1 尺寸/内存预算超限风险**
- 症状/根因：APP1 分区 7,143,424 字节，固件已到 ~4.2MB（58%+）；RAM 静态 81–82KB；不设上限会撞分区。
- 解决/规避：验证门禁 RAM ≤35%、APP1 ≤85%（`scripts/check_m4_build_budget.py`）；每次构建报告 size/SHA。（M4_ARCH_ACCEPTANCE_MATRIX.md、GROK_RC2_FINAL_REVIEW.md）

**[F-6] 恢复 APP1 旧 bin 不等于回 APP0**
- 症状/根因：restoring 旧 APP1 `.bin` 不会选择 APP0 启动槽。
- 解决/规避：用 `--select-slot 0` 或恢复 helper 创建的 otadata 备份。（MURPHY_M4_APP1_FLASH.md）

---

## 三、SD 卡 / 文件系统

**[S-1] 插件安装：`sd_write: SD 写入失败` / `upload_begin: 无法创建暂存文件` / 随机 `missing:<file>: 解压失败`（P0，2026-08-01 真机阻塞）**
- 症状：同一包 Wi-Fi 传输报 `sd_write: SD 写入失败`；USB 分片报 `[M4Dbg] Failed to open file for writing: /apps_inbox/weread.m4x.part`（`openFileForWrite` 直接失败）；解压阶段随机报 `missing:content_provider.lua` / `missing:api.lua`（每次缺失文件不同）。SD 刚格式化、容量基本为空，读取一切正常（`sd_ok:true`，书籍/缓存/历史都能读）。
- 根因：① `SDCardManager::openFileForWrite` = `vol().open(path, O_RDWR|O_CREAT|O_TRUNC)`，无全局 SD 互斥、无写探针、无落盘校验；② `M4SerialDebugBridge.cpp` USB 上传（540-563/608-620）不上传结束时显式 `sync()`、忽略 `close()` 返回值，SHA-256 是对**收到的内存分片**计算而非对 SD 重读文件，`finishUploadAndInstall()` 关文件后直接 rename 解压不重读校验；Wi-Fi 下载路径（867-930）同样不检查 sync/close 返回值；③ `M4xInstaller.cpp`（106-133）对截断/读取不一致的 ZIP 逐个 `getInflatedFileSize()/readFileToMemory()` → 表现为随机 `missing:<file>`；④ 瞬态文件系统/介质状态问题（格式化 + 重试可恢复）。
- 解决/规避：① 结论上"安装失败 ≠ 未更新"——同一包格式化 SD/重试后可成功（实测 wifi 传输一次成功）；install 报错后必须回查设备状态（重装看 `noop:true` 还是 `install ok`）；② `status` 的 `sd_ok:true` 只证明可读/已挂载，**不能证明可写**；读取正常+写入失败 ≠ "SD 满"，先怀疑瞬态 FAT/介质状态；③ 修复方向（P0）：加 `sd_probe` 命令（写 4KiB 多块→sync→close→重读逐块校验→返回各阶段结果+FAT 类型+总/可用空间）；上传完成后 sync/close 检查返回值、从 `.part` 重读算 SHA 与主机声明比对、失败返回 `sd_verify_failed` 而不是伪装 `missing`；P1：SD 访问统一互斥、失败清理 `.part/.bak/.staging`、ZIP 错误分类（`zip_open_failed`/`zip_central_dir_failed`/`zip_entry_short_read`/`zip_inflate_failed`/`zip_crc_mismatch`）；验收：空 SD 连续安装同包 10 次全成功。（M4_ISSUE_SD_INSTALL_FAILURE_20260801.md、M4_UPDATE_BLOCKED_SD_WRITE_20260801.md、M4_PLUGIN_UPDATE_DEEPSEEK_GUIDE.md）

**[S-2] 无法从 host 侧查 SD 剩余空间**
- 症状/根因：调试桥 op 只有 ping/status/launch/tap/key/back/home/screenshot/install/install_http/logs，**没有 df/space 类命令**，`status` 不含 SD 剩余空间，无法确认是否"满"。
- 解决/规避：待办（P0 建议加 sd_probe 返回总/可用空间）；本地先按"格式化+重试"处理。（M4_UPDATE_BLOCKED_SD_WRITE_20260801.md §1.3/§2）

**[S-3] `fs.replaceFile` 非原子 → 断电丢最后好状态**
- 症状/根因：实现先写 `.tmp`，删除 live 文件，再 rename tmp；断电在删除后即丢失最后好状态；copy fallback 也在新副本完成前删除 live 路径。
- 解决/规避：改为 `live → .bak`、`tmp → live`、再删 `.bak`；失败恢复 `.bak`；boot 后确定性恢复 `.tmp/.bak`。（GROK_RC2_FINAL_REVIEW.md P0）

**[S-4] 状态文件恢复把"合法空文件"当损坏、把"部分副本"当好文件**
- 症状/根因：`M4StateFile::Snap` 用 `liveNonEmpty/bakNonEmpty`，合法原子替换为空串被当作缺失，旧 `.bak` 会回灌陈旧数据；改成 `liveExists` 后又丢失"完整 vs 部分"区分——FAT fallback 下 `tmp→live` 复位会留下 `live=部分新副本 + bak=完整旧 + tmp=完整新`，`decide()` 只要 live 存在就 `UseLiveDropTmp`，下次读到截断 JSON 并把完整 tmp 丢掉；restore copy fallback 忽略短读/短写失败后可能删备份。
- 解决/规避：区分存在性与非零长度（空状态合法）；`live+bak+tmp` 视为未完成 copy 事务，恢复已知好备份；检查精确 read/write/rename/remove 结果；无法证明成功时保留 `.bak/.tmp`。（GROK_RC2_FINAL_REVIEW.md 第三/四/五轮 P1/P0）

**[S-5] 全局安装 journal 自身断电不安全**
- 症状/根因：`M4xInstallJournal::writeAll()` 写 `<journal>.tmp`→删除当前 journal→rename/copy；在删主文件与装新文件之间复位会丢全部未完成事务；`loadAll()` 只读主文件，不复核对 `.tmp/.bak`。
- 解决/规避：用可恢复的 primary/backup/temp 协议替换；完整替代品存在前不得删最后一个有效 journal；load 时校验并对齐三份。（GROK_RC2_FINAL_REVIEW.md 第三轮 P0）

**[S-6] 恢复逻辑无条件清记录 / 备份被无依据删除**
- 症状/根因：`recoverAll()` 忽略 `dropStaging/restoreOldFromBak/dropBak` 及最终 `saveAll(remaining)` 的返回值，`clear=true` 无条件置位——恢复失败时删掉 journal 记录而 live app 缺失/不完整；首次安装 registry 提交失败只返回 `ClearJournalOnly`，把重试所需元数据丢了；`RegistryCommitted` 且 `registryMatchesNew==false` 时 `DropBakClearJournal`——注册表缺失/损坏会导致删除唯一旧副本而不重交新元数据；`hookRestoreOld()` 在证明备份可恢复前就删新 live 树；`hookDropBak()` 备份还在也报成功。
- 解决/规避：除非该恢复分支所需全部动作成功否则保留记录；首次安装 registry 失败应保留 `LiveSwitched` 下次 boot 重试；恢复前先证明/复制备份到独立可恢复目标再切换；cleanup hooks 校验后置条件并返回真实失败。（GROK_RC2_FINAL_REVIEW.md 第三轮 P0）

**[S-7] 安装器即时错误分支绕过安全恢复实现（重复的破坏性算法）**
- 症状/根因：`M4xInstaller::install()` 几个即时错误分支（LiveSwitched journal 更新失败后删新 live 树、不查结果就 rename/copy 备份、无条件删 journal；registry save 失败重复同样序列；promoteStaging 失败仅凭"假设恢复成功"删 journal）与 boot 恢复 hooks 算法不同且更危险——一次失败的 SD rename/copy 可导致既无 live app 也无恢复记录。
- 解决/规避：即时失败处理复用同一验证过的 restore helper 与后置条件；仅当验证恢复成功（旧条目在且 registry 一致）才删 journal；否则保留原 journal 阶段与全部可恢复产物供 boot 重试。（GROK_RC2_FINAL_REVIEW.md 第四轮 P0）

**[S-8] 回滚成功后残留 `LiveSwitched` journal → 下次 boot 用新元数据盖旧文件**
- 症状/根因：`handleInstallFailRestore()` 记录 `snap.bakExists` 后调 `hookRestoreOld()` 再刷新 `snap.bakExists`——成功恢复消耗掉备份，`bakExists` 变 false；而 `mayClearJournalOnInstallFail()` 以 `if (!s.bakExists) return false` 开头，导致成功回滚正常无法清 journal；registry-save 失败场景持久记录仍是 `LiveSwitched`，而 live 文件与 registry 已恢复为旧版；下次 boot `decideRecovery(LiveSwitched)` 看到 live 目录就把**新** registry 元数据提交到**旧**文件上。测试漏掉是因为其"verified restore"快照保留 `bakExists=true`，不是 `hookRestoreOld()` 的真实后置。另：handler 忽略 `M4xInstallJournal::remove()` 失败也返回成功。
- 解决/规避：不能用"当前备份存在"证明回滚完成可清记录，应单独记录恢复前状态；回滚后绝不留描述新文件的 `LiveSwitched` 持久阶段；journal 移除/更新失败必须有 boot 安全结果。最终采用 fail-closed 设计：即时安装失败不改变/不清除任何东西，保留最后持久阶段与产物，交给集中式 boot 恢复收尾。（GROK_RC2_FINAL_REVIEW.md 第五轮 P0）

**[S-9] 升级 quarantine 用新 manifest 处理旧文件**
- 症状/根因：`quarantineLive()` 收到 incoming manifest 并用其 `entry/icon/files[]` 复制现有安装；新版本新增/改名文件在旧安装中不存在 → 文件级 quarantine 失败；被删除的旧文件无法清理；这在 FAT 上目录 rename fallback 时尤其关键。
- 解决/规避：从现有 registry 条目取旧清单用于 quarantine/restore/备份清理；新 manifest 只用于 staging/promote。（GROK_RC2_FINAL_REVIEW.md P0）

**[S-10] 安装事务无崩溃恢复 / promote 后立即删备份**
- 症状/根因：`M4xInstallTxn::txnMarkerPath()` 未使用，无阶段标记、boot 不扫描 `.staging/.bak`；quarantine 后断电 live app 缺失；live 切换后断电出现新文件配旧 registry 元数据；`promoteStaging()` 在 `M4xRegistry::save()` 成功前就删备份 → registry 失败时旧版不可恢复；旧目录→备份 rename 失败时 `promoteStaging()` 直接拷贝新文件覆盖 live 安装，中途失败旧 app 损坏且无备份；file promote 成功后 registry 保存失败 = 新文件配旧元数据。
- 解决/规避：实现持久化阶段 + boot 恢复；备份保留到 registry 提交成功；各阶段确定性恢复；FakeFs 测试必须执行真实安装器代码而非重实现算法。（GROK_RC2_FINAL_REVIEW.md P0、GROK_RC2_REVIEW_BLOCKERS.md P0）

**[S-11] `removeTreeBestEffort()` 递归清理不完整**
- 症状/根因：只删已知文件并仅尝试第一个父目录段；app-data clear 只删几个常见文件，不删嵌套的 WeRead 章/目录缓存；registry 不持久化 manifest `files[]`，卸载只知 entry/icon；清理旧备份时用新 manifest。
- 解决/规避：根受限递归枚举器（最深优先删空父目录）；`clearData=true` 必须删全部 app data；卸载删所有 app 文件；测试多层路径与缓存树。（GROK_RC2_FINAL_REVIEW.md P1、GROK_RC2_REVIEW_BLOCKERS.md P1）

**[S-12] `Storage.write_json()` 非原子**
- 症状/根因：最终 `fs.writeFile()` 在成功可知前替换 live 文件，无 rename 原语暴露。
- 解决/规避：宿主文件 API 增加安全 rename/replace 并用于状态。（GROK_RC2_REVIEW_BLOCKERS.md P1）

**[S-13] SD boot 报通用 `SD Card Error`（卡在位）**
- 症状：卡在位但 UI 显示 `"SD card error"`；失败只输出 `[M4-SD] ERROR: SD card initialization failed`，丢失阶段信息（host_init vs sector vs volume）；mount 后无能力探测。
- 解决/规避：`SdmmcBlockDevice` 记录 host_init/slot_init/card_init/no_card/sector_timeout|io 分类；`SDCardManager` 映射 stage/code 名；boot 做只读 capability probe（root 列表 + 无害文件 open/read/seek）；区分无卡/mount 失败/FS 不支持/I/O 失败/成功；**boot 期间不创建探测文件**；有界重试，禁止格式化/分区/删用户数据。（MURPHY_M4_RC1_PHASE3_LOG.md §2、GROK_TASK_MURPHY_M4_RC1_PHASE3.md）

**[S-14] 大书缓存构建吃满内存**
- 症状/根因：`BookMetadataCache::buildBookBin`/`load` 曾用全文件 `std::vector<uint8_t>(fsize)`（X3/C3 + 大 M4 缓存会爆）。
- 解决/规避：改为 `validateBookBinStreaming` 流式校验（O(1) RAM）；`book.bin.tmp` + `acceptAsComplete` 校验 + rename；`load()` 拒绝截断结构与孤儿 tmp。（MURPHY_M4_RC1_PHASE3_LOG.md Corrective pass 2/3）

**[S-15] 进度/索引缓存损坏导致灾难**
- 症状：损坏 `progress.bin`（`chapter=12032, page=6661`）曾触发空批次全文件扫描 10–30s + "Empty file"；`progress.bin` 每页写失败（`Failed to open for writing: .../progress.bin`，同目录章节缓存可读）。
- 根因：损坏进度未校验越界；`openFileForWrite` 用 `O_RDWR|O_CREAT|O_TRUNC` 每页失败（目录可写性/占用/FAT/路径是目录/open 文件数未定论）。
- 解决/规避：进度加载时 clamp/校验（日志 `progress chapter 12032 invalid → 0`）；缓存发布原子化、损坏缓存安全重建一次不无限重试；TIDX 用 magic/version/size/layout fingerprint，`complete=0` 的 partial 文件绝不当作最终。（task_prompts/analyze_m4_open_book_hang_20260731.md、WEREAD_0_5_NATIVE_READER.md）

**[S-16] 下载事务：`.part/.dlbak` 约定**
- 症状/根因：下载直接写目标文件，断电留半截。
- 解决/规避：先写 `.part`，同步、关闭、尺寸校验后再发布；复位留下 `.dlbak` 时启动恢复旧 live 文件。（M4_REUSABLE_ARCHITECTURE.md）

---

## 四、WiFi / 网络 / TLS

**[N-1] 文件传输/网页活动后"无网络"，WeRead 直接离线**
- 症状：RC3 真机：安装/文件传输后 WeRead 立即报无网络；中文 UI 全是 `?`。
- 根因：`CrossPointWebServerActivity::onExit()` 调用 `WiFi.disconnect(false)` 且常 `WiFi.mode(WIFI_OFF)`；`AppRuntimeActivity`/`M4xLuaHost` 从不重连 STA；`net.isConnected()` 只读 `WiFi.status()`（正确，不能说谎）；WeRead `load_shelf()`/`Auth.begin_login()` 把"未连接"当终态离线，不尝试已存凭据。
- 解决/规避：宿主新增 `net.connectSaved([timeout_ms]) -> {ok,error,ssid?}`（错误码 `no_saved_wifi|timeout|cancelled|connect_failed`），WeRead `ensure_network()` 在离线 UI 前先重连；**所有权规则**：宿主只在自己建链时拥有（`wifiOwned_`），`stop()` 仅在自己拥有时 `disconnect(false)`；M4x 路径禁止 `disconnect(true)`（擦凭据）；错误区分（no-saved/timeout/TLS/HTTP 不合并成"无网络"）。（M4_WEREAD_RC3_REAL_DEVICE_FIX.md、task_prompts/grok_m4_weread_rc3_real_device_fix_20260730.md）

**[N-2] `net.request` 对 keep-alive 响应读到超时（挂起）**
- 症状/根因：读取循环 `while (http.connected() || stream->available())`，Arduino `HTTPClient::connected()` 在底层 keep-alive 连接存活时恒真；已知 Content-Length 时没在解完 body 字节后停止；`getStreamPtr()` 路径无被证实的 chunk 解码器。
- 解决/规避：已知长度就读满即停；chunked 走框架解码或实现有界 chunk 解码并单测；connection-close 体在断开时停并同时执行 idle/total deadline；测试用真实本地 HTTP server 覆盖 keep-alive/Content-Length/chunked/redirect/timeout/oversize。（GROK_RC2_REVIEW_BLOCKERS.md P0）

**[N-3] HTTP grow 适配器丢失 body 偏移（截断/损坏响应）**
- 症状/根因：`M4xHttp::Buffer::len` 解码中前进；grow 时 `netBodyBufGrow()` 用 `gGrowTarget->len` 重置 `self->len`，而 `gGrowTarget->len` 读期间未同步恒为 0 → 超过初始 4096 字节预留的 chunked/until-close 体会覆盖早期字节并报截断/损坏 body。测试预分配足够空间且用不同 grow 回调，覆盖不到。
- 解决/规避：grow 保留 `self->len` 并精确拷贝；删除全局 `gGrowTarget` 回调状态，改为带 context 的 adapter；生产等价多 chunk + >4096 until-close 测试。（GROK_RC2_FINAL_REVIEW.md P0）

**[N-4] 重定向 Set-Cookie 被丢弃 / 下一跳不用**
- 症状/根因：`respHeaders.clear()` 每跳都清，重定向响应在被结束前 Set-Cookie 未保留（部分认证流在重定向时设置/轮换 cookie）；后续修复只把 cookie 累积给最终 Lua 结果，内部下一跳请求在 Lua 看到前已发出 → 需要新 cookie 的重定向目标仍失败。
- 解决/规避：同源重定向跳累积允许的 Set-Cookie；跨 full origin（scheme/host/effective port）绝不转发 Cookie/Authorization；`mergeSetCookiesIntoRequestHeaders` 合并进下一同源请求；加重定向 cookie 集成测试。（GROK_RC2_FINAL_REVIEW.md P1、第三轮）

**[N-5] 登录覆盖真实 `wr_skey` cookie / 续期假成功**
- 症状/根因：`auth.lua` 先吸收 Set-Cookie 再无条件 `c.wr_skey = token`，而 accessToken 不保证是 wr_skey；`Auth.has()` 未校验最小可用身份（wr_vid+wr_skey）；`try_renew()` 因 `... or true` 对任何 HTTP 2xx 返回 true，续期失败也当成功。
- 解决/规避：保留服务端 Set-Cookie，token 仅无 cookie 时兜底；校验续期 JSON/结果码，拒绝时返回 false；Lua 级 fixture 覆盖登录 cookie 优先级/被拒续期/成功续期/缺身份字段。（GROK_RC2_REVIEW_BLOCKERS.md P0）

**[N-6] 大响应超上限：`response_too_large`（正文失败 reader 200.0）**
- 症状：WeRead v0.4.2 真机报 `正文失败 reader 200.0`；真实 reader 响应 HTTP 200 但 wire 约 894KB，解码文本约 822KB，`"psvts"` 出现在 ~238KB 处；`M4xNetPolicy::kMaxBodyWithPsram` = 768KB，`l_net_request()` 对已知超限 Content-Length 直接拒绝 → `{status=200, ok=false, error=response_too_large}`。
- 根因：整页 HTML 无法进 Lua（堆限 512KB），也不能为它提高通用 cap。
- 解决/规避：新增 `net.extractPsvts()` 流式扫描（有界状态机，scan cap 2MiB、value cap 512 字节，跨任意 chunk 边界处理引号值；稳定错误 `psvts_not_found|psvts_unclosed|psvts_value_too_large|scan_too_large|timeout|idle_timeout|cancelled|https_required`）；保持 TLS CA 校验/HTTPS-only/重定向/Cookie 剥离策略；不提高通用 body cap。（task_prompts/grok_m4_weread_stream_psvts_20260730.md、M4X_APPS.md）

**[N-7] TLS CA bundle 缺 ZeroSSL 证书，握手失败 -30336**
- 症状/根因：设备 CA bundle 不含 ZeroSSL（fanqie 镜像 `fq-book.nat.netsite.cc:8043` 用 ZeroSSL ECC DV SSL CA 2），验证握手必失败（-30336）。
- 解决/规避：按 app 分支：无凭证 app（fanqie）`setInsecure`，有 Cookie 的（weread）保持校验。（M4_DEVELOPMENT_NOTES.md §7）

**[N-8] 跨源重定向 Cookie 泄漏 / 无重定向凭据策略**
- 症状/根因：初始实现盲目跟随重定向，`Cookie/Authorization` 可能跨源转发；后来实现只有 full-origin 比较才允许。
- 解决/规避：比较 scheme/host/effective port 全源后才转发；`net.request` 手动跟随重定向（可剥 Cookie）；jsonGet 用 `FORCE_FOLLOW_REDIRECTS`。（M4X_WEREAD_REWRITE_REVIEW.md P0、M4_DEVELOPMENT_NOTES.md §7）

**[N-9] 陈旧 `psvts` 响应被当终态**
- 症状/根因：stale `psvts` 返回 HTTP 200 但空/无效 payload 被当终态，不刷新一次。
- 解决/规避：在可识别认证失败与无效/空内容时刷新一次。（GROK_RC2_REVIEW_BLOCKERS.md P1）

**[N-10] 同步进度上传阻塞 UI（最多 15s）**
- 症状/根因：进度上传是未验证的同步请求，在章开/退时执行，可阻塞 UI 达 15s。
- 解决/规避：默认禁用（`Api.ENABLE_PROGRESS_UPLOAD=false`）直到签名协议有验证 fixture；绝不阻塞绘制；分页完成后才保存进度，上传失败不影响阅读。（GROK_RC2_REVIEW_BLOCKERS.md P1、weread_incremental_pagination_rc4.md）

**[N-11] 微信读书协议：用错域名 / 分片解码错**
- 症状/根因：`i.weread.qq.com` 需额外签名易 401，必须走 `https://weread.qq.com/web/*`；`-2012` = 登录超时；`wr_skey` 短效、`wr_rt` 长效；e_0/e_1/e_3 解码 swap 为 bit 展开（`1<<(2*b)`），错误实现会乱码。
- 解决/规避：协议要点写入文档；续期走 `POST /web/login/renewal`；cookie 轮换落盘 appdata。（WEREAD_M4X_ARCHITECTURE.md §2.2/§2.3）

**[N-12] 同步 HTTP 叠加重试卡 UI**
- 症状/根因：同步请求 + 重试叠加会卡 UI。
- 解决/规避：宿主单次超时可控；插件取消/阶段提示；网络任务延迟一帧（先渲染 loading 反馈，下一帧 draw 里执行同步 fetch）。（WEREAD_M4X_ARCHITECTURE.md §2.3、M4_FANQIE_PAGING_FIX_20260803.md §3.1）

**[N-13] 大响应禁止 Arduino `String` 硬塞**
- 症状/根因：大响应塞进 Arduino String 会耗 RAM/碎片。
- 解决/规避：宿主 `net` 用 `std::string`/PSRAM，上限并报错。（WEREAD_M4X_ARCHITECTURE.md §2.3）

**[N-14] Wi-Fi 传输的陈旧 IP/首请求超时**
- 症状/根因：旧固件在文件传输会话有 stale-IP/首请求超时问题。
- 解决/规避：固件等待非零 DHCP 地址，并在传输会话禁用 Wi-Fi 电源保存（`wifi_prepare`/`wifi_transfer`）。（M4_SERIAL_DEBUG_BRIDGE.md）

---

## 五、Lua 沙箱 / 内存

**[L-1] 网络体上限与 Lua 堆上限自相矛盾**
- 症状/根因：宿主允许 512KiB（无 PSRAM）/1.5MiB（有 PSRAM，后改 768KiB）响应并整体拷贝进 Lua 字符串；Lua allocator 总量仅 512KiB（含脚本、表、既有 reader 状态）且用普通 realloc；大响应在 HTTP 层通过却在 Lua 无法表示；JSON 解码还需更多内存。
- 解决/规避：按当前 Lua 堆 headroom 限制返回体（`pushNetResult` 中 `≤¾ headroom`，超限返回 `response_too_large_for_lua`，不损坏 allocator）；或流式写 app-data 文件增量解码；测试经 `pushNetResult` 而非只测独立 body reader；报告典型章节的峰值 internal heap/PSRAM/Lua heap。（GROK_RC2_FINAL_REVIEW.md P0、M4X_WEREAD_REWRITE_REVIEW.md）

**[L-2] 整章进 Lua 内存：SD 缓存只省下载不省内存**
- 症状/根因：`Storage.load_chapter_text()` 仍 `fs.readFile()` 整章为 Lua 字符串；512KiB 堆下任何单章大文件都爆；分页索引"一行一个 table"无限增长。
- 解决/规避：`fs.fileSize(rel)` + `fs.readRange(rel, offset, length)`（length 1..16384 硬上限，绝不超 length、EOF 短读、路径沙箱、不复制整文件）；下载成功写盘后置 `text=nil` 主动回收；页首 byte offset 紧凑结构或流式 `.pidx`，Lua 只留当前页/邻页游标；UTF-8/CRLF 跨窗口保证前进不丢/重字节；单次 draw/tick 的读取、扫描、textWidth 有硬上限。（task_prompts/weread_sd_window_reader_phase5a.md）

**[L-3] 整章排版超预算：`callback time budget exceeded`（layout.lua:139）**
- 症状：点击"第一章 童年"先显示"加载章节…"随后宿主错误页 `layout.lua:139: callback time budget exceeded`；错误码 139 不是 HTTP 状态。
- 根因：`open_chapter()` 在单次 `onTouch()` 内同步完成取正文+写缓存+整章分页+存/上传进度；`M4xLuaSandbox::kDefaultWallMs` = 8000ms；失败发生在整章排版阶段。
- 解决/规避：重写为增量状态机——`open_chapter()` 快速切 loading 并返回；分页分批推进（字节/行/时间片上限）；loading 可触摸返回目录；空正文/超长单行/CRLF/非法截断 UTF-8 不得死循环；清理临时正文/分页状态；**禁止以提高全局 8s 预算代替状态机修复**；版本 0.4.4/versionCode 8。（task_prompts/weread_incremental_pagination_rc4.md）

**[L-4] ArduinoJson 默认分配器走内部 RAM → 持续翻页后重启**
- 症状：解析 87KB 书单响应建 ~150–200KB 池，把 287KB 内部堆顶到 38KB 空闲 → 持续翻页后设备重启。
- 根因：ArduinoJson 默认分配器在内部 RAM 分配。
- 解决/规避：必须用 PSRAM allocator（`JsonDocument doc(PsramJsonAllocatorInstance())`）；`min_free_heap` 是金标准——掉到几十 KB 就怀疑内部堆被大块占用。（M4_DEVELOPMENT_NOTES.md §7）

**[L-5] 宿主投影是唯一正确姿势**
- 症状/根因：书单 87KB/页、TOC 数百 KB；`json.decode` 大响应会爆堆/长阻塞（任务看门狗）。
- 解决/规避：大 JSON 走 `dl.jsonGet`/`dl.jsonToFile` 宿主侧解析，Lua 只拿小结果/文件路径；`M4xJsonStream::RecordExtractor` 按结构路径增量解析直写事务 sink，不聚合 500KiB 目录进 Lua 堆；`json.decode` 前按 `len > headroom*3/4` 拒绝（返回 `nil,"json_too_large"`）；`sys.memInfo()` 重操作前自查（`ensure_mem(65536)`）。（M4_DEVELOPMENT_NOTES.md §6、M4X_PLUGIN_UI_SCENE.md §3、M4_REUSABLE_ARCHITECTURE.md）

**[L-6] Lua 翻页 OOM / 内存 guard 误报**
- 症状/根因：Lua 同时持有解码表+书单+UI rows 三份 → OOM；内存 guard 误报。
- 解决/规避：每页只驻留当前页，旧页交给缓存引用，`collectgarbage` 及时回收。（M4_DEVELOPMENT_NOTES.md §6）

**[L-7] 宿主回调抛错直接杀死 app**
- 症状/根因：宿主 pcall 调 Lua 回调，错误 → `setFailed` → `renderError` → app 退出 Home。
- 解决/规避：任何回调（onTouch/onKey/on_row/on_page/draw）内不许抛错，宁可在 Lua 里 pcall 包住；所有全局状态变量在声明处初始化（曾有 `attempt to perform arithmetic` → 退回 Home：`booklist_page` 只在旧宿主回退分支赋值）。（M4_DEVELOPMENT_NOTES.md §6）

**[L-8] Lua 沙箱不是强安全边界**
- 症状/根因：自定义 allocator 512KiB + 指令步数/墙钟预算（`lua_instr_limit`/`lua_time_limit`）是资源隔离，恶意插件仍可滥用网络/长 HTTP 阻塞。
- 解决/规避：文档明确"非对抗恶意插件的强隔离"；目标是保护主阅读器免于常见插件故障。（M4X_APPS.md）

**[L-9] 沙箱超限 → "应用运行失败"退出**
- 症状/根因：Lua 沙箱超预算/超时 → 宿主抛错 → AppRuntime 显示"应用运行失败"并退出插件（不拖死整机）；单次 draw/回调内超长阻塞会触发。
- 解决/规避：插件避免单次 draw/回调做超长阻塞操作；延迟网络任务一帧化。（M4X_PLUGIN_UI_SCENE.md §3）

---

## 六、阅读器 / 分页

**[R-1] 打开 TXT 卡死/长时间无响应（真机串口日志分析）**
- 症状：打开《信息全知者.txt》(8.9MB GBK) 后卡死一阵才有反应；打开到首帧结束 ≈ 2.5s（onEnter 写 RecentBooks ≈1s；首帧 e-ink+AA ≈1.5s）；此后每 1.4–1.6s 一次完整整页重绘（BW FAST → Wait 505ms → Store BW → Wait 61ms → Restore → progress.bin FAIL → CPS save），体感像卡死；主循环翻页/点菜单也要 `lockState` → 墨水屏刷新期间主线程无法处理输入。
- 根因：`displayTaskLoop()` 持 `renderingMutex` 调 `renderScreen()`，内部 `displayBuffer(FAST)` 等 BUSY ~500ms + 灰度双 pass + `saveProgress()` 写 SD（当前失败）+ 每帧 `APP_STATE.saveToFile()`；主线程被锁阻塞；Plugin 首帧已把 HALF 刷新挪到锁外，**库内 TXT 路径没有**；损坏 progress（chapter=12032/page=6661）曾触发空批次全文件扫描 10–30s + Empty file。
- 解决/规避（待办方向）：区分首帧延迟 vs 交互卡死 vs 章节元数据扫描三类根因；不把 e-ink BUSY 与 mutex 持有时间混为一谈；修复 progress.bin 写失败路径（O_WRONLY/目录可写性/FAT 检查）；目录跨 25 章 batch + 持锁 scan + 选章 `lockState(portMAX_DELAY)` 曾死锁/假死。（task_prompts/analyze_m4_open_book_hang_20260731.md）

**[R-2] 中文长段落折行被丢弃 → 一行溢出**
- 症状/根因：`Layout.paginate()` 只存页字节区间；`Layout.page_text()` 返回原始子串；`UiReader.draw()` 只按字面换行切分 → 长中文段落画成一行溢出；`test_utf8_layout.cpp` 在 C++ 里复制数学未执行 layout.lua。
- 解决/规避：存每页行字节区间并渲染精确行；绝不拆分 UTF-8 序列；clamp 到当前段落；生产 Lua 测试覆盖长 CJK 段/混合 ASCII/标点/空段/页边界 + headless 行宽断言。（GROK_RC2_REVIEW_BLOCKERS.md P0）

**[R-3] 每个换行插一个空行（TXT 双倍行距）**
- 症状/根因：段落折行后循环到达终止换行时又推入一个空视觉行 → `"a\nb"` 变成 a、空行、b；RC2 截图只有约一半行被用。
- 解决/规避：单个换行终止当前行，仅额外空行增加空白行；`a\nb`、`a\n\nb`、CRLF、中文段落精确行测试。（GROK_RC2_FINAL_REVIEW.md P1）

**[R-4] 页面基数不一致：native 0 基 vs Lua 1 基**
- 症状/根因：`TxtReaderActivity::PluginProgress.page` 是 0 基；WeRead Lua `reader_page`/持久化 `page` 是 1 基；`onReaderClosed()` 直接 `reader_page = prog.page` → native 第 0 页变成非法 0 页，完成百分比差一。
- 解决/规避：边界只转换一次：bridge 契约保持 0 基；Lua 展示/持久化 `reader_page` 与 `native_progress.page` 1 基；`byteOffset` 保持权威用于 native 重开；测试 page 0→显示/持久 1、native 4→Lua 5（含百分比）。（task_prompts/grok_m4_weread_050_postaudit_deadlock_pagebase_20260730.md）

**[R-5] byte 进度保存了但从不恢复**
- 症状/根因：Lua 保存 `native_progress.byteOffset`，但 `open_native_reader()` 不传它，bridge/request/session 无初始 byte offset，`TxtReaderActivity` 从不恢复。
- 解决/规避：end-to-end restore——`initialByteOffset` 经 openText→OpenRequest→PluginSession 传递并在 native 校验（对 fileSize）；有完整 `.tidx` 时先二分查该字节所在页；无完整索引仍先渲染第 1 页、保留 pending restore offset，渐进索引到达时恰好选一次并刷新；用户手动翻页后清 pending；切书/切章时 reset `native_progress` 防陈旧数据写错章节；byte offset 比页码更能承受分页/字体变化。（task_prompts/grok_m4_weread_050_final_audit_fixes_20260730.md）

**[R-6] 首帧等待整章索引 / 首帧延迟（loading 页滞留）**
- 症状：真机：点章后物理屏停在 Lua loading 页很久，用户误报崩溃；堆从 ~170KiB 掉到 ~104KiB 再恢复 ~155KiB（无重启）；`step_chapter_load()` 在 draw() 内同步做 fetch/缓存/打开，`onEnter()` 索引 1 页后子循环还额外索引 2 页才 render；`pluginProgressSnapshot()` 只等 100ms，e-ink 渲染持锁更久 → 返回伪造的 page-0 默认快照并保存。
- 根因：UX 不区分下载/写缓存/native-open 排队/native 首帧；父 Lua 在 openText 被接受后仍可能用陈旧 loading 帧覆盖 native 首帧。
- 解决/规避：首帧优先——cache hit 不得先 Lua 分页或全章扫描再 handoff；native 只排版第 1 页先物理刷新，其余有界后台切片；切章到 frontier 时同步 catch-up 一段；快照等锁超时不得返回伪造默认值（等待当前渲染/索引片完成可接受，或返回 Lua 不持久化的显式失败）；TIDX 每轮只存一次完成索引。（task_prompts/grok_m4_weread_050_first_frame_latency_20260730.md、grok_m4_weread_050_production_sync_fix_20260730.md、grok_m4_weread_050_audit_rework_20260730.md）

**[R-7] 番茄读书分类书单无法翻页（点"下一页"无反应）**
- 症状：分类浏览→书单进入后无法翻页。
- 根因：旧 host 不解析 `ui.listOpen` 的 `page_count/initial_page/remote_page` 三字段，按本地驻留行数推算总页数：分类书单每页只驻留 6 本 → `totalPages==1`；`uiCallPage` 对目标页 `clamp(page,total)`：2→1 与当前页相同 → 契约"clamp 后页不变→不回调" → `on_book_page` 永不触发。
- 解决/规避：host 完整解析三字段（`page_count>0` 优先于按行数推算，`<1` 视为 0，`>4096` clamp；`page_size<3`→12、`>40`→40）；`newPage==当前页 → 直接 return 不回调`（末页再翻是静默 no-op）；`uiCallRow` 远端模式 localIndex 解析回驻留行、回调仍传全局逻辑 index0；插件 `local_idx = idx % BOOKLIST_FETCH_SIZE + 1`；`has_next = #list >= 6`，最后一页 partial 不再 +1。（M4_FANQIE_PAGING_FIX_20260803.md）

**[R-8] 翻页几页后状态错乱（footer 页码 ≠ 内容）/ 取消翻页后翻页永久失效**
- 症状/根因：loading 屏 tap 在 fetch 已进行时把 `screen` 误设 `toc`，与后台 fetch 完成的开场景打架；`booklist_loading` 未复位。
- 解决/规避：区分 pending job / in-flight，in-flight 时忽略 tap；cancel 路径重置标志位。（M4_DEVELOPMENT_NOTES.md §6）

**[R-9] 切章后状态栏标题仍显示上一章**
- 症状/根因：原生阅读器无缝切章后标题未更新（固件 bug，2026-08-01 修复清单第①项）。
- 解决/规避：改为从 toc.json 解析章节标题。（M4_UPDATE_BLOCKED_SD_WRITE_20260801.md §0）

**[R-10] 插件进度污染系统历史/最近阅读**
- 症状/根因：插件会话也写 `progress.dat`（Home 阅读历史进度随翻页/切章更新）——插件缓存章不应进 Recent Books；plugin 路径同时调普通 `loadProgress()/saveProgress()`，与 Lua/native 进度矛盾。
- 解决/规避：`PluginTxtReaderActivity` 不得 `RECENT_BOOKS.addBook`；插件 bridge 的 raw-byte 进度权威，不用库 `progress.bin`；读统计可选但不污染全局/库 last-book 状态。（WEREAD_0_5_NATIVE_READER.md、task_prompts/grok_m4_weread_050_final_audit_fixes_20260730.md）

**[R-11] GBK/UTF-16 被当 UTF-8 原始字节渲染（乱码）**
- 症状/根因：自定义 `PluginTxtReaderActivity` 调 `Txt::readContent(..., false)` 后把原始字节当 UTF-8 解析/渲染，绕过 `TxtReaderActivity::loadPageAtOffset()` 的编码边界/解码逻辑 → GBK 与 UTF-16 损坏、BOM/边界错乱。
- 解决/规避：复用真实 TXT reader 布局路径，保留 UTF-8/GBK 流式解码/UTF-16LE/BE 流式解码与"解码字符→原始字节偏移"精确映射；集成级分页/渲染边界测试。（task_prompts/grok_m4_weread_050_audit_rework_20260730.md）

**[R-12] TXT 编码支持边界（RC2 Phase 4B）**
- 症状/根因：RC1 时 `Txt::load()` 总是整文件 GBK→`utf8.txt` 全量转换；`Txt::isGbkEncoding()` 硬返回 false → GBK 从不流式；无 UTF-16 BOM 路径；GB18030 四字节无映射。
- 解决/规避：`directTxtRead=1` 直读原文件，不建 UTF-8 副本/侧车；流式解码器带 carry；GB18030-4byte/Unknown → **拒绝打开 + 诊断弹窗**（不静默 mojibake、无 U+FFFD 流路径）；多点多段采样检测（head≤1024 + mid/tail≤256 + align pad）；`nextOffset` 用绝对 raw end，绝不用比例估算、不跳过长 GBK 行。（MURPHY_M4_RC2_PHASE4B_LOG.md、MURPHY_M4_RC2_PHASE4B_DESIGN.md）

**[R-13] 嵌套菜单/子活动从自身回调里自毁（UAF 风险）**
- 症状/根因：`TxtReaderActivity::openMenu()` 回调在 `EpubReaderMenuActivity` 回调栈内调 `exitActivity()`；`PluginTxtReaderActivity::loop()` 调 `onClose_`，AppRuntime 回调里 `exitActivity()` 在子活动 loop/callback 仍在栈上时 reset/delete 子活动 → use-after-free / 销毁活动中的 `std::function`；`pumpSubActivityFrame()/requestExitSubActivity()` 机制未被使用（`loop()` 手动调 `subActivity->loop()` 并 return）。
- 解决/规避：两阶段关闭——子活动只发布关闭请求；父活动在 `subActivity->loop()` 返回后关子、再投递 Lua 进度事件；回调请求 deferred 关闭而非内联 reset；生命周期回归测试覆盖嵌套菜单场景。（task_prompts/grok_m4_weread_050_audit_rework_20260730.md、grok_m4_weread_050_final_audit_fixes_20260730.md）

**[R-14] 插件独立阅读器 ≠ 复用系统阅读器**
- 症状/根因：`PluginTxtReaderActivity` 是第二个简化阅读器：固定 `NOTOSANS_16_FONT_ID`、无系统菜单/设置/进度样式、无书签/字体/行距/方向、center touch 只设 `updateRequired_` 不开菜单、自定义英文 footer。
- 解决/规避：重构真实 `TxtReaderActivity` 或提取共享 native reader core，插件章用同一渲染/字体设置/菜单/触摸区/视觉风格；transient launch 策略抑制 RecentBooks/全局 last-opened/库扫描器。（task_prompts/grok_m4_weread_050_audit_rework_20260730.md）

**[R-15] 进度上传用陈旧 Lua 分页总量算百分比**
- 症状/根因：`onReaderClosed()` 拷 `prog.page`，但 `try_upload_progress()` 从旧/空 `reader_page_starts/reader_pages` 算百分比，忽略 `prog.total`/`prog.byteOffset` → 百分比为 0 或错。
- 解决/规避：持久化含 chapter UID + 稳定 byte offset/percent 的 native 进度记录；`prog.total` 仅在 complete 时用；按 byte offset 恢复；应用进度前校验 bookId/chapterUid/progressKey 精确匹配（不许 `gotKey==""` 放行）。（task_prompts/grok_m4_weread_050_audit_rework_20260730.md、grok_m4_weread_050_production_sync_fix_20260730.md）

---

## 七、插件安装

**[P-1] `noop:true` 语义误判**
- 症状/根因：安装器按 versionCode 比较；设备已装同版本时返回 `noop:true`；把"待安装目录里的旧包"误报为设备已装版本；只看 `apps_inbox` 包不能判断设备当前版本。
- 解决/规避：以 `m4adb install` 返回的 manifest/`status` 为准；`noop:true` 表示已相同或更新，不是失败；修改插件源码后必须 bump `manifest.json` 的 version/versionCode，否则版本比较无意义、内容更新被静默忽略（`same version ≠ same content`，watch 流程曾被破坏）。（M4_AI_UPDATE_GUIDE.md §3.1/§7.4、task_prompts/grok_m4_serial_debug_review_fixes_20260730.md P0-1）

**[P-2] `.m4x` 包与源目录失同步**
- 症状/根因：weread.m4x 曾停留在 0.6.2 而源码到 0.6.8；只改源目录却装旧包。
- 解决/规避：发版前 `unzip -p pkg manifest.json` 核对；`test_m4x_package_parity` 逐字节比对包与源；优先让 m4adb 从源目录用 Python 标准库打包，避免手工 ZIP 格式差异；manifest `files[]` 每个文件必须在 ZIP 中存在（特别是 api.lua/content_provider.lua）。（M4_DEVELOPMENT_NOTES.md §8、M4_PLUGIN_UPDATE_DEEPSEEK_GUIDE.md）

**[P-3] 安装器路径穿越 / zip bomb / 只解压清单文件**
- 症状/根因：旧实现 `dest + entryName` 无 normalize → 路径穿越；`readFileToMemory` 无上限 → zip bomb；只解压 manifest/entry/icon，`sys.load` 看不到其它模块。
- 解决/规避：allow-list 解压（manifest+entry+icon+files[]）；路径规范化拒绝绝对路径/`..`/`\`/控制字符/过长路径；上限 manifest≤16KiB、entry≤256KiB、其它≤512KiB、总解压≤2MiB、≤64 文件；staging→validate→rename 切换；registry `.tmp`+`.bak`。（M4X_WEREAD_REWRITE_REVIEW.md P0、M4X_APPS.md）

**[P-4] 上传协议：overflow 帧必须整行丢弃 / Base64 严格校验 / chunk 元数据安全**
- 症状/根因（review 发现）：`feedByte()` 超长后 `lineLen_==kMaxLineLen`，换行时可能把截断的前 kMaxLineLen 字节交给 `handleLine()` 执行；Base64 解码器跳过所有非法字节当"噪音"；设备忽略 `total`，不拒绝 `seq>=total`、跨 chunk 不一致 total、声明未到齐就 commit；重试同一 chunk 会中止上传而非重放 ack。
- 解决/规避：显式 `discardUntilNewline_` 状态；严格 Base64 字母表/填充校验（host `validate=True`）；校验 total/seq 一致性；chunk 幂等重放 ack；host 超时重试同 request ID 有界次数；已完成 request ID 下不接受不同 mutation。（task_prompts/grok_m4_serial_debug_review_fixes_20260730.md）

**[P-5] 安装任务跑后台 FreeRTOS task → 竞态**
- 症状/根因：`debugInstallTask` 后台任务与 UI/runtime 并发用 SD、竞争 `std::string`/`M4xInstallResult`/volatile 标志；规范禁止 UI/installer 生命周期操作来自后台任务。
- 解决/规避：删除该任务与共享跨核结果状态；`M4xInstaller::install` 调度到主 owner loop（同步命令可接受，host 已有长安装超时）。（task_prompts/grok_m4_serial_debug_review_fixes_20260730.md）

**[P-6] 安装中残留 `.part/.bak/.staging`**
- 症状/根因：失败路径不清理；旧 inbox 包可能在 staged 包 rename 成功前被销毁。
- 解决/规避：失败必须关闭文件并清理 `.part/.bak/.staging` 与 journal 未完成记录，清理失败记录具体路径不静默吞掉；rename 失败时保留/恢复可恢复副本。（M4_ISSUE_SD_INSTALL_FAILURE_20260801.md、task_prompts/grok_m4_serial_debug_review_fixes_20260730.md）

**[P-7] `l_reader_openText` 忽略 queueOpen 返回值**
- 症状/根因：`M4PluginReaderSession::queueOpen()` 被拒绝时仍返回成功。
- 解决/规避：检查并返回结果，绝不 reject 后报成功；native open/load/encoding 失败必须向 Lua/UI 返回显式错误，不能伪装成第 1 页正常关闭。（task_prompts/grok_m4_weread_050_final_audit_fixes_20260730.md）

---

## 八、模拟器

**[M-1] 模拟器与生产行为脱节（最被反复批判的坑）**
- 症状/根因：ctest 全绿但生产路径未被执行：`M4IndexStatePublication` 只被单测引用、生产仍有真实数据竞争；测试用 C++ 重实现算法（test_utf8_layout、MemFs::replaceFileProd）而非执行生产代码；HTTP grow 测试预分配够内存且用不同回调；install txn 测试的 FakeFs 重实现算法；"verified restore"快照的 bakExists 与 `hookRestoreOld()` 真实后置不符。
- 解决/规避：测试必须进入生产路径；模拟器 mock 必须复刻宿主 clamp/no-op 语义（如 uiCallPage 末页静默 no-op、uiTapRow index0 映射），否则测不到回归点；不用静态源码字符串断言冒充覆盖。（GROK_RC2_FINAL_REVIEW.md、GROK_RC2_REVIEW_BLOCKERS.md、M4_FANQIE_PAGING_FIX_20260803.md §4.2、task_prompts/grok_m4_weread_050_production_sync_fix_20260730.md）

**[M-2] 模拟器中文栅格化错位/重叠（ctest 却绿）**
- 症状：journey_home.png/library.png 中文碎裂重叠。
- 根因（vs `GfxRenderer::renderChar`）：① host blit 用行填充 `rowBytes=(width+7)/8` 每行重启位，EPDF 是连续 `pixelPosition=y*width+x` 流（生产按 `bitmap[pixelPosition/8]`、位 `7-(pixelPosition%8)`）；② host 用 `y0=originY+top-height`，生产是 `screenY=baselineY-glyph->top+glyphY`；③ host 2-bit 路径错误掉进坏的 1-bit 行解码器。
- 解决/规避：可移植 `epd_glyph_raster.h`（镜像生产采样/放置，不链接进固件）；golden PBM + 密度/不重叠断言 + 连续 vs 行打包发散断言（字形"中"）。（MURPHY_M4_SIMULATOR_PHASE2_LOG.md）

**[M-3] 延迟一帧的网络任务测试必须 pump**
- 症状/根因：插件网络任务第 1 帧提交 loading、第 2 帧才执行同步 fetch；同步断言会在 loading 态失败。
- 解决/规避：用 `pumpDraws(L,1)/pumpDraws(L,2)` 驱动；`test_fanqie_mem` 同步调用断言全部改 pump。（M4_FANQIE_PAGING_FIX_20260803.md §4.1/§4.4）

**[M-4] Lua C API 负索引栈越界 SIGSEGV**
- 症状：`test_fanqie_mem` 曾 SIGSEGV（t=函数指针）。
- 根因：`lua_rawseti(L,-2,key)` 假定栈顶是 table 且下面还有元素；栈上只有 1 个元素时 `-2` 越过栈底，取到残留函数指针当 table 用。
- 解决/规避：构造结果表时先 `lua_newtable` 垫底，再逐元素 newtable/setfield/rawseti（`-2` 始终安全）。（M4_FANQIE_PAGING_FIX_20260803.md §4.3、M4_DEVELOPMENT_NOTES.md §6）

**[M-5] AppleClang 16 + CLT libc++ 不兼容**
- 症状/根因：macOS SDK 的 libc++ 拒绝 `__builtin_ctzg/__builtin_clzg`（AppleClang + CLT SDK 不匹配）。
- 解决/规避：用完整 Xcode 工具链或 Homebrew gcc-14 配置 simulator（`run_m4_simulator.sh` 自动选 Xcode）。（MURPHY_M4_NATIVE_SIMULATOR.md、M4_ARCH_ACCEPTANCE_MATRIX.md）

**[M-6] 模拟器不做电学/时序建模（边界声明）**
- 症状/根因：不建模 SSD1677 电学时序、波形、残影物理、功耗、深睡、SDMMC 信号完整性、前光电子；EPUB 是 fixture 路径非完整 parser；frontlight 只是数值 UI 状态。
- 解决/规避：文档明确非目标；硬件项留在真机 checklist。（MURPHY_M4_NATIVE_SIMULATOR.md、GROK_TASK_M4_NATIVE_SIMULATOR_PHASE2.md）

---

## 九、并发 / 死锁 / 竞态

**[C-1] 单 Lua VM 被并发进入（双任务破坏 lua_State）**
- 症状/根因：`displayTaskLoop()` 只给 `host_.callDraw()` 加 `renderingMutex_`，`loop()` 的 `callOnKey()/callOnTouch()` 不加；`draw()` 还做同步轮询/网络；`failed_/error_/updateRequired_` 跨任务无同步方案 → 同一 lua_State 被两个 FreeRTOS 任务进入。
- 解决/规避：所有 Lua 回调在唯一 owner task（输入事件队列）；若保留 mutex 必须覆盖每个 Lua 入口与全部共享状态，teardown 不得在任务持锁/等锁时删任务。（GROK_RC2_REVIEW_BLOCKERS.md P0）

**[C-2] 非递归互斥重入死锁（多处同型）**
- 症状/根因①：`displayTaskLoop()` 拿 `renderingMutex` 后 `renderScreen()` 处理 `m_pendingJumpPercent` 调 `goToPercent()` 再拿同一非递归 mutex → 库内书签跳转路径可永久挂起。
- 症状/根因②：目录跨 25 章 batch + 持锁 scan + 选章 `lockState(portMAX_DELAY)` 曾死锁/假死。
- 解决/规避：重构锁定/非锁定调用点显式化且不可误用；禁止改用递归 mutex；锁序明确、不引入渲染/状态 mutex 反转；真机日志中避免在 e-ink 刷新期间持状态锁。（task_prompts/grok_m4_weread_050_postaudit_deadlock_pagebase_20260730.md、analyze_m4_open_book_hang_20260731.md）

**[C-3] 运行时创建失败 → 退出死等**
- 症状/根因：queue/mutex 创建或 `xTaskCreate()` 失败时无 owner task，`OwnerLifecycle::ownerDone` 永远不发布；`onExit()` 仍 `requestStopAndJoin()` 永久等待。
- 解决/规避：跟踪 owner task 是否创建成功；未创建则不清等直接退出并安全清理部分分配资源；加 queue/mutex/task 创建失败测试。（GROK_RC2_FINAL_REVIEW.md P0）

**[C-4] 渐进索引状态跨任务数据竞争（多轮 audit P0）**
- 症状/根因：display task 追加/realloc `pageOffsets`、更新 `totalPages/indexComplete_`，UI task 在翻页/菜单/关闭快照/进度查询时无锁读写 `pageOffsets/currentPage/totalPages/indexCursor_/indexComplete_/pending restore/userMovedPage_/hasPendingRestore_`；`pluginProgressSnapshot()` 100ms 超时返回伪造默认值；display-loop 条件在外读 `indexComplete_/firstPageReady_`。
- 解决/规避：生产与测试共享同一状态对象（或每处生产访问统一状态锁）；无锁时不许 vector 访问；翻页决策+变更、pending-restore 应用/取消、菜单/进度读取各自相干；锁序明确；避免在 e-paper `displayBuffer()` 期间持状态锁；触摸翻页可短暂等待有界索引片（正确性优先于理论非阻塞）。（task_prompts/grok_m4_weread_050_production_sync_fix_20260730.md、grok_m4_weread_050_final_audit_fixes_20260730.md）

**[C-5] 安装器与主页扫描/缓存写/字体任务并发访问 SD**
- 症状/根因：无全局 SD 互斥；安装任务、主页扫描、缓存写、字体/阅读器后台任务并发访问同一 FsVolume。
- 解决/规避：跨任务写与"写后读校验"统一 mutex；禁止只在调用方局部加锁。（M4_ISSUE_SD_INSTALL_FAILURE_20260801.md P1）

---

## 十、性能 / 延迟

**[T-1] 首帧延迟 2.5s：onEnter 写 RecentBooks ≈1s + 首帧 e-ink+AA ≈1.5s**
- 症状/根因：`TxtReaderActivity` 打开首帧 11505→14031ms；主线程 onEnter 里 `CPS Saving state` 与 `RBS Recent books saved` 之间 ~949ms 阻塞；e-ink BUSY（BW FAST 505ms + 灰度 61ms + AA 双 pass）全在渲染线程锁内。
- 解决/规避：Plugin 首帧把 HALF 刷新挪出锁外（库内 TXT 路径待同改）；首帧只排版第 1 页先显示，后台有界切片续索引。（task_prompts/analyze_m4_open_book_hang_20260731.md、grok_m4_weread_050_first_frame_latency_20260730.md）

**[T-2] 快速连点触发 loading 屏/取消逻辑**
- 症状/根因：一次书单 fetch 约 3–4s；tap 间隔 <2.5s 会踩 loading 屏；快速连点触发取消。
- 解决/规避：验证节奏用单连接脚本 + 每 1.5s tap；产品逻辑上 in-flight 时忽略 tap。（M4_DEVELOPMENT_NOTES.md §5、§6）

**[T-3] 连续整页重绘风暴（体感卡死）**
- 症状/根因：`updateRequired` 连续触发 + 每帧 `APP_STATE.saveToFile()` + `saveProgress()`（失败）→ 每 1.4–1.6s 一次完整渲染周期。
- 解决/规避：减少每帧状态写盘；进度写失败需根因修复；background 只更新状态不重绘未变正文。（task_prompts/analyze_m4_open_book_hang_20260731.md、M4_PLUGIN_PROVIDER_AUDIT_GUIDE.md §4）

**[T-4] 大目录/整本拷贝进 vector**
- 症状/根因：宿主或 `TxtReaderActivity` 把整本目录复制进 `std::vector`；`Catalog.virtual_rows` 未被使用则 `#chapters` 全 UID 驻留。
- 解决/规避：FileRows 宿主只读当前可见页；chapterCount 已验证作为列表长度；`M4ContentProviderSession::BookState` 稀疏存章节状态（1928 行目录只留 1 个 ChapterStatus）。（M4_PLUGIN_PROVIDER_AUDIT_GUIDE.md §3、M4_CONTENT_PROVIDER_ARCH.md）

**[T-5] 宿主 theme 每行查询全局**
- 症状/根因：每列表行查全局 UITheme。
- 解决/规避：`M4UiStyleAdapter::current()` 场景进入/旋转时取一次；Theme 无内部堆分配 O(1)。（M4_UI_STYLE_ARCH.md、M4_REUSABLE_ARCHITECTURE.md）

**[T-6] 会话缓存窗口（正向经验）**
- 症状/根因：书单回翻重新拉网络慢。
- 解决/规避：已访问书单页缓存 8 页窗口（~1KB/页），回翻秒开零网络；换分类清空。（M4_DEVELOPMENT_NOTES.md §6）

---

## 十一、字体 / 渲染

**[FONT-1] 中文 UI 全 `?`（RC3 真机）**
- 症状：安装后 WeRead 中文界面全是 `?` 替换字形。
- 根因：M4 构建 `OMIT_FONTS`：内置 `m4_ui_cjk_*` 只是小 UI 子集（~760 codepoints），不是完整 WeRead chrome；完整 CJK 需 SD 上规范字体 `/fonts/NotoSansCJKsc.epdfont`（release 产物 `artifacts/fonts/release/`，~1.3MiB）。
- 解决/规避：`EpdFontLoader::loadFontsFromSd()` 存在且有效时把该字体提升到 NOTOSANS reader/app-content ID；紧凑 UI_10/UI_12/SMALL 保持内置（避免固定 16px 字形挤压系统菜单）；**不要把 1.3MiB 字体塞进 APP1**；缺字体时系统画布回退内置子集 + `sys.fontInfo()` 诊断，插件不得硬阻塞；安装脚本 `scripts/install_canonical_cjk_font.sh`；版本 0.4.0/versionCode 4 → 0.4.1/5（`gui.lineHeight()` 适配 45px 规范度量）。（M4_WEREAD_RC3_REAL_DEVICE_FIX.md、M4X_APPS.md）

**[FONT-2] 字体缺失时正文变 `?` 而非诊断**
- 症状/根因：无 SD 字体时缺字形渲染为 replacement/`?`。
- 解决/规避：M4FontPolicy：只自动提升规范字体；PREFLIGHT 校验 header + 期望 SHA-256（`44b5164bb1dd1f59e9230a5c81383a6dc7a5e8559103fbf4bbd0a69b224919f4`），缺失/无效 → 诊断 + 安全回退；不宣称 runtime TTF，RC1 用固定 16pt epdfont（GB2312-plus，非全罕见/繁体 CJK）。（MURPHY_M4_RC1_PHASE3_LOG.md §6、M4_DEVELOPMENT_NOTES.md §1）

**[FONT-3] `.cpfont` 未实现 / `OMIT_FONTS` 误读**
- 症状/根因：本树不实现 `.cpfont`；`OMIT_FONTS` 被误解为"无中文 UI"——它只是省略多 MiB 完整 Noto 表。
- 解决/规避：UI 中文走内置子集；整书中文走 SD epdfont；文档明确。（MURPHY_M4_PHASE1_IMPLEMENTATION_LOG.md）

**[FONT-4] 字体大小/行高变化使分页失效**
- 症状/根因：阅读中改字体设置后旧索引失效。
- 解决/规避：TIDX 带 layout fingerprint（size/family/编码/版本），不匹配则安全重建；设置变化使渐进索引失效重建并保留 byte-offset 恢复语义。（task_prompts/grok_m4_weread_050_production_sync_fix_20260730.md P1、WEREAD_0_5_NATIVE_READER.md）

---

## 十二、安全 / 凭证

**[SEC-1] 日志/报告泄漏凭证**
- 症状/根因：`journey.py` 声称 scrub 日志但写原始行；真机验证文件 `build/weread_live_probe/session.json` 含 cookie/token。
- 解决/规避：集中 redactor 在写 `serial.log`/timeline/report/Markdown 前覆盖 Authorization/Bearer、Cookie/Set-Cookie、密码、token、API key、WeRead session-like cookie、URL query 密钥；保留有用错误上下文；发布前脱敏 docs/task_prompts（个人路径 `/Volumes/z`、`/Users/...`、设备 IP、实验字体文件，示例 IP 改 192.0.2.x）；测试断言 artifact 目录无密钥值。（task_prompts/grok_m4_serial_debug_review_fixes_20260730.md P1-7、M4_DEVELOPMENT_NOTES.md §9）

**[SEC-2] 调试桥开关只能本地开，且默认关**
- 症状/根因：早期拆 `murphy_m4_debug` 双固件方案被否；桥字符串"二进制不存在"≠安全，安全属性是"运行时默认关"。
- 解决/规避：单一 `murphy_m4` 固件含桥，由设备上 开发者选项→USB 串口控制（默认 off）控制；off 时有界丢弃 CDC RX、解析器保持 reset、使能时 flush（关闭期间收到的字节不得在使能后执行）；上传中关闭必须安全中止并清 `.part`、重置 SHA/chunk/parser/幂等状态、停止 keep-alive；无任何串口/RPC 能远程开授权；X3/X4 不编译桥。（task_prompts/grok_m4_runtime_developer_option_20260730.md、M4_SERIAL_DEBUG_BRIDGE.md）

**[SEC-3] 沙箱路径边界**
- 症状/根因：Lua 提供全路径不可信；`fs.*`/`reader.openText` 需要路径沙箱。
- 解决/规避：`reader.openText` 只允许 app data 内规范相对路径，拒绝绝对路径/`..`/symlink 逃逸/他 app/非普通文件/错误扩展名（仅 `.txt`）；`fs.fileSize/readRange` 用 `sandboxDataPath()`；宿主原生解析校验，绝不信任 Lua 全路径；FAT 无实用 symlink 但文档注明限制。（task_prompts/grok_m4_weread_native_reader_progressive_20260730.md、weread_sd_window_reader_phase5a.md）

**[SEC-4] 安装器安全边界**
- 症状/根因：桥只允许固定 staging `/apps_inbox/`；需拒绝路径穿越/绝对路径/超大包/坏 hash/部分传输/降级尝试。
- 解决/规避：按 `M4xInstaller` 策略校验 app/包标识与文件名；队列/行长/解码 payload/chunk/每主循环处理时间有界；畸形输入拒绝不崩溃不无限分配。（task_prompts/grok_m4_serial_debug_mode_20260730.md）

---

## 十三、工具链 / 环境 / 流程

**[E-1] 系统 python 缺 `pyserial`/`esptool`**
- 症状：m4adb / otatool / 一切串口脚本用系统 python（`/opt/anaconda3/bin/python3`）直接失败。
- 解决/规避：一律用 `~/.platformio/penv/bin/python`（两者都有）。（M4_UPDATE_BLOCKED_SD_WRITE_20260801.md §1.5、M4_DEVELOPMENT_NOTES.md §1）

**[E-2] 诊断脚本被 kill 留半开连接**
- 症状/根因：kill 后串口被占、设备可能卡异常状态。
- 解决/规避：操作前 `lsof /dev/cu.usbmodem*` 检查，残留进程先 pkill。（M4_AI_UPDATE_GUIDE.md §7.2）

**[E-3] 安装解压期间串口刷屏 `E task_wdt: esp_task_wdt_reset(707): task not found`**
- 症状/根因：`M4xInstaller.cpp` 在未注册 watchdog 的任务里调 `esp_task_wdt_reset()`。
- 解决/规避：已知无害噪音，不影响安装，可顺带修掉。（M4_AI_UPDATE_GUIDE.md §7.5）

**[E-4] 会话/版本纪律：一次会话一个 transport、版本 bump、parity**
- 症状/根因：多进程连打、不 bump 版本、包源不同步是反复出现的坑。
- 解决/规避：boot drain → wait_ready → install → status → close 一次会话；插件改源码必须 bump versionCode；发版前核对 manifest 与 parity 测试；固件/插件对真机验证顺序：模拟器回归（ctest）→ 编译 → 真机；每个新构建单独授权。（M4_AI_UPDATE_GUIDE.md、M4_PLUGIN_UPDATE_DEEPSEEK_GUIDE.md、M4_FANQIE_PAGING_FIX_20260803.md §5）

**[E-5] 发布卫生：不提交生成物/密钥/本地路径**
- 症状/根因：生成目录、serial 抓包、session JSON、`.m4x` 包、个人路径易入库。
- 解决/规避：发布前 `git diff` 检查意外改动/密钥；secret scan + clean `git status`；`simulator/build-gcc14/` 等 gitignore；vendor 参考仓不进主仓（LICENSE 不清晰不整包拷贝）。（task_prompts/grok_m4_serial_debug_mode_20260730.md、WEREAD_M4X_ARCHITECTURE.md §7）

**[E-6] x3_default 构建预存故障**
- 症状/根因：`pio run -e x3_default` 在 `FontCacheManager.h` / `esp_partition_mmap_handle_t` 兼容性处失败（与 BookMetadataCache 流式化无关）；X3/X4 不编译桥。
- 解决/规避：明确出作用域不修；文档注明。（MURPHY_M4_RC1_PHASE3_LOG.md、M4_SERIAL_DEBUG_BRIDGE.md）

**[E-7] 回滚/历史纪律（AGENTS 层）**
- 症状/根因：反复 open/close 复位、并行写同一工作区、静默分派任务到别的模型。
- 解决/规避：单 owner、先停再报；所有固件改动先模拟器/主机回归；刷写只用 APP1 安全脚本；未授权不刷 APP0/bootloader/分区表/NVS；真机尽量单持续 USB 会话。（AGENTS.md、M4_FANQIE_PAGING_FIX_20260803.md §5）

---

## 十四、其他（协议/状态机/过程）

**[O-1] `missing:<file>` 只允许表示 central directory 真缺条目**
- 症状/根因：SD 短读、inflate 失败、CRC/大小不匹配都被折叠成 `missing:<file>`，且每次缺失文件不同，误导排查方向。
- 解决/规避：独立错误码 `zip_open_failed`/`zip_central_dir_failed`/`zip_entry_short_read`/`zip_inflate_failed`/`zip_crc_mismatch`；`sd_verify_failed` 独立于 ZIP 错误。（M4_ISSUE_SD_INSTALL_FAILURE_20260801.md P1）

**[O-2] 权限检查只验错误串不够**
- 症状/根因：断言"有错误串"但注入网络/文件系统计数非零。
- 解决/规避：测试必须断言权限失败时注入的 network/filesystem 调用计数保持为 0（jsonGet 无 network 权限在 URL 解析/分配/socket 前拒绝；download 无 appdata 权限在创建 `.part` 前拒绝）。（M4_ARCH_ACCEPTANCE_MATRIX.md §3）

**[O-3] 空 UID 请求网络**
- 症状/根因：FileRows 目录 pollWork 初始 `chapterUid` 为空，插件若直接拿空 UID 请求网络即废请求。
- 解决/规避：`requiresCatalogResolve(work)` 显式化；`provider.resolveCatalogWork` 唯一行解析入口；空 UID 绝不发往网络端点。（M4_CONTENT_PROVIDER_ARCH.md）

**[O-4] 内存验收必须分域报告**
- 症状/根因：把内部 RAM/PSRAM/Lua 堆加总隐藏 ESP32-S3 上真实失效模式。
- 解决/规避：分域报告与门禁：静态/内部 RAM ≤35%；APP1 ≤85%（硬限 `0x6d0000`）；流式 JSON/下载瞬时内部堆 ≤24KiB、文件列表分页 ≤16KiB；PSRAM 响应窗口不镜像进内部堆；Lua 堆 ≤512KiB 且 2000 行操作后 headroom ≥64KiB。（M4_ARCH_ACCEPTANCE_MATRIX.md §2）

**[O-5] 真机现象 vs 模拟器假设（最高优先证据）**
- 症状/根因：模拟器绿但真机出问题（RC2 安装成功但无网络/`?`；RC4 超预算）。
- 解决/规避：以物理失败为权威（"physical failures are authoritative"）；真机验证至少覆盖扫码/session 恢复、书架、目录触摸、缓存章直开、切章、返回桌面、错误后资源释放；宿主固件与插件必须成对验证，插件升级不能代替宿主修复。（M4_WEREAD_RC3_REAL_DEVICE_FIX.md、M4_AI_UPDATE_GUIDE.md §4）

---

# 高频坑 TOP 15（按出现频次 / 严重度排序）

| # | 坑 | 出现文档数 | 严重度 |
|---|----|-----------|--------|
| 1 | **串口 open/close 触发 `USB_UART_CHIP_RESET` → 不断重启 / 完全静默（需断电）** | 8+（GUIDE、OPS、BRIDGE、BLOCKED、DEVELOPMENT_NOTES、APP1_FLASH、BLOCKERS 系列） | P0 操作级 |
| 2 | **SD 写入失败：`sd_write` / `upload_begin 无法创建暂存文件` / 随机 `missing:<file> 解压失败`；`sd_ok:true`≠可写** | 5（ISSUE、BLOCKED、GUIDE、DEEPSEEK、FINAL_REVIEW） | P0 阻塞真机 |
| 3 | **非递归 mutex 重入死锁（renderingMutex×goToPercent；displayTaskLoop 持锁做 e-ink/输入）** | 4（HANG、POSTAUDIT、FINAL_REVIEW、AUDIT_REWORK） | P0 死锁 |
| 4 | **安装事务非原子/断电恢复缺陷（journal 自身、清记录、删备份、LiveSwitched 残留、错误分支绕恢复）** | 2 个超长 review 系列（FINAL_REVIEW 五轮 + REVIEW_BLOCKERS） | P0 数据破坏 |
| 5 | **Lua 512KiB 堆 vs 网络响应/整章内存（`response_too_large`、`json_too_large`、超预算 `callback time budget exceeded`）** | 6（FINAL_REVIEW、STREAM_PSVTS、SD_WINDOW、RC4、UI_SCENE、REWRITE_REVIEW） | P0 崩溃/失败 |
| 6 | **渐进索引跨任务数据竞争 + 100ms 快照伪造 page 1 + 测试模型未接生产** | 4（SYNC_FIX、FINAL_AUDIT、POSTAUDIT、REVIEW_BLOCKERS） | P0 数据竞争 |
| 7 | **HTTP 读取挂起/损坏：keep-alive 恒真读到超时；grow 丢 `len` 截断 body；重定向 cookie 丢弃/下一跳不用** | 3（REVIEW_BLOCKERS、FINAL_REVIEW、REWRITE_REVIEW） | P0/P1 |
| 8 | **WiFi 断开不重连 → WeRead 永久"无网络"；所有权（wifiOwned_）与 `disconnect(true)` 红线** | 3（RC3_FIX、RC3 任务、M4X_APPS） | P0 功能 |
| 9 | **中文字体缺失 → `?`；规范 epdfont 契约与 1.3MiB 不入 APP1** | 4（RC3_FIX、PHASE1_LOG、RC1_LOG、M4X_APPS） | P0 显示 |
| 10 | **打开书卡死/首帧 2.5s：e-ink BUSY 持锁、每帧 saveToFile、损坏进度→全文件扫描 10–30s、progress.bin 写失败** | 3（HANG 任务、RC1_LOG、RC2_PHASE4B） | P0 体感崩溃 |
| 11 | **页面基数 0 基/1 基错位（`reader_page = prog.page`）与 byteOffset 存而不复** | 3（POSTAUDIT、FINAL_AUDIT、AUDIT_REWORK） | P0 进度错误 |
| 12 | **otatool/系统 python 环境：`No module named esptool`、`sys.executable` 拉起、rich_click 缺** | 3（BLOCKED、GUIDE §7.1、DEVELOPMENT_NOTES） | P0 刷机阻塞 |
| 13 | **模拟器与生产脱节：测试重实现算法/只用测试模型/mock 不复刻 clamp-no-op 语义** | 4（FINAL_REVIEW、SYNC_FIX、FANQIE_FIX §4.2、REVIEW_BLOCKERS） | P0 假绿 |
| 14 | **GBK/UTF-16/GB18030 编码处理错误（按 UTF-8 渲染乱码、GB18030 四字节静默错误、`utf8.txt` 全量转换）** | 3（AUDIT_REWORK、PHASE4B_LOG/DESIGN） | P0/P1 乱码 |
| 15 | **插件升级静默失效：不 bump versionCode → `noop:true`；同版本≠同内容；包/源失同步（weread.m4x 0.6.2 vs src 0.6.8）** | 4（GUIDE §7.4、DEEPSEEK、DEVELOPMENT_NOTES §8、SERIAL_REVIEW_FIXES） | P1 发布事故 |

---

**补充说明（避免误读）：**
- 分类间有重叠（如"安装事务"同时涉及 SD 卡与插件安装），条目已按主根因归位并在文中交叉引用。
- 未标注"解决/规避"的条目（如 S-2 无 df 命令、部分 HANG 待办）在文档中仍属待办或分析中状态。
- 所有涉及具体文件/函数名/错误码的细节均已保留在条目正文中，可直接作为最终交付文档素材。
</task_result>
</task>