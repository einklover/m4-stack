# Murphy M4 翻页动画（Waveform Lab）任务交接文档

> 交接日期：2026-08-06
> 分支：`agent/runtime-ttf-psram`（固件仓库）+ `grok/m4-device-rc1`（SDK 仓库）
> 交接对象：后续接手翻页动画/墨水屏波形优化的 AI

---

## 1. 任务目标

在 Murphy M4（ESP32-S3 + SSD1677 800×480 墨水屏）上实现 **Kindle 式翻页动画**：
- 翻页时新页从右向左"擦入"（wipe），而非瞬间切换
- 通过自定义 LUT（波形）控制每步刷新速度与残影
- 最终集成到 EPUB/TXT 阅读器的翻页流程

## 2. 已完成的工作

### 2.1 Waveform Lab（USB LUT 实验平台）— 固件侧完整可用

隐藏的 USB 调试功能（无 UI 入口），通过 M4SerialDebugBridge 串口协议操作：

| 命令 | 功能 |
|---|---|
| `lut_begin {slot,size}` | 开始帧上传（分片写 PSRAM 槽位） |
| `lut_upload {lut,b64 110B}` | 上传实验 LUT（电压尾默认锁定） |
| `lut_set_frames {prev,next}` | 指定 SD 上的前后帧路径 |
| `lut_baseline {frame}` | FULL 全刷到指定帧（建立差分基线） |
| `lut_run {swap}` | 执行一次 prev→next 差分刷新，返回 BUSY ms |
| `lut_animate {prev,next,steps,feather}` | **设备端即时合成** wipe 动画（PSRAM 计算，无逐帧传输） |
| `lut_swap` / `lut_stats` / `lut_clear` / `lut_end` | 交换/状态/安全恢复/退出 |

**代码位置**：
- `src/debug/M4WaveformLab.h/.cpp` — 实验服务（PSRAM 帧槽、SD 帧、LUT、合成动画）
- `src/debug/M4SerialDebugBridge.cpp/.h` — `lut_*` 命令分发 + 分片上传
- `lib/hal/HalDisplay.h/.cpp` — `waveformLabRefresh/Baseline` 透传
- SDK（`open-m4-sdk/`，独立 git 仓库）：`FreeInkDisplay.cpp/.h` 加 `waveformLabRefresh/Baseline/powerOnIfNeeded`；`Ssd1677Driver` 加 `powerOnIfNeeded`
- `scripts/waveform_lab.py` — 电脑端工具（帧上传、LUT、wipe 动画）
- `scripts/page_turn_gen.py` / `scripts/page_wipe_gen.py` — 截图→物理帧/擦入帧生成

### 2.2 已验证的关键结论（真机实测）

1. **帧格式**：800×480 1bpp = 48000 字节，**0=黑、1=白**（PBM 1=黑，需反转）
2. **逻辑↔物理方向**：逻辑 480×800 portrait → 物理 800×480 需旋转（`X=log_h-1-y, Y=x`）——真机验证正确
3. **差分刷新基线**：FAST 差分只驱动 RED≠BW 的像素。baseline 后若面板断电（FULL 自断电），下次 FAST 会被提升为 HALF/FULL → 需 `powerOnIfNeeded`
4. **LUT 结构**（110B）：前 105B = VS 波形（00/01/10/11/VCOM 各 10B）+ TP/RP 时序（50B）+ 帧率（5B），后 5B = 电压尾（VGH/VSH1/VSH2/VSL/VCOM）
5. **binary 差分 LUT**（参考 `lut_x4pro_partial`）：
   ```
   00: 不驱动  01: 0x80 强驱动（黑→白）
   10: 0x40 强驱动（白→黑）  11: 不驱动
   TP=0x30/RP=0x01 单相位, 帧率 0x22, 电压尾用 M4 安全值 17 41 A8 32 30
   ```
6. **TP 时长扫描**（真机）：0x30→1061ms、0x08→261ms、0x04→181ms、0x02→142ms、0x01→121ms
7. **方向验证**：全黑→全白（01 方向）✅、全白→全黑（10 方向）✅、四象限 00/11 不动 ✅
8. **CDC 缓冲坑**：`ARDUINO_USB_CDC_RX_BUFFER_SIZE` 编译宏**无效**（该 Arduino core 忽略），必须运行时 `Serial.setRxBufferSize(8192)`（已在 main.cpp setup 加）——否则长 JSON 命令（lut_upload 148 字符 b64）在 e-ink BUSY 期间丢帧

### 2.3 过程中修复的其他固件 bug（顺带）

- `NetworkModeTask` 栈 2048→4096（进入网络设置界面必崩）
- 晋江正文 `bad_params`（宿主 jsonGet 拒绝空 path 根投影）
- 微信读书书架 200+ 本 `response_too_large`（jsonGet 192KB 硬上限→4MB，PSRAM 钳 768KB）
- 番茄章节"只显示 4 页"（early_bytes=2048 早开 reader 后不重分页 → pendingComplete 占位 + 完成重开）
- 三插件 progress.json 全量读写 → 按书分片 `progress/<bookId>.json`

## 3. Kindle 式多趟驱动（已实现语义，待真机调参）

### 3.1 正确差分语义（2026-08-06 修正）

**错误旧路径**：每步 `RED=上一步 synth`、`BW=当前 synth`  
→ 已覆盖区 RED=BW=page2 → **不再驱动** → 像素只变一次，无"变重"。

**Kindle 目标路径**：
```
每步 i，edge 从右向左扫：
  RED = 原始 page1（始终）
  BW  = composeWipe(page1, page2, edge)   // 左 page1，右 page2
```
| 区域 | RED | BW | 驱动 |
|---|---|---|---|
| 未扫到 x < edge | page1 | page1 | 否 |
| **已覆盖 x ≥ edge** | **page1** | **page2** | **每步都驱** |

- 边界继续左移 → 擦入动画
- 已覆盖区每步用**真实前后页**再驱动 → 墨粒多趟沉降 → 视觉"变重"
- **不是** 4 级灰阶 LUT 淡入淡出（那是另一条路）

实现：
- `runAnimate` / async mode 0：全帧 multipass（RED 固定 page1）
- `runAnimateWindow` / mode 1：窗口 multipass，每步刷新整段已覆盖 `[edge, W]`（不再只刷新增窄条）
- 动画结束后可用 `runSettle`（更强多相位 LUT）做最终固化

### 3.2 仍需真机调参

- 步数 / TP：建议 6–12 步、TP=0x04..0x08，总时长 ~1–2s
- feather 边缘抖动：0–16 px
- settle 相位：1–3
- 窗口 vs 全帧：窗口早期 SPI 更少；全帧路径更简单

### 3.2 其他遗留

- `lut_upload` 偶发超时仍需客户端重试（已加 5 次重试，建议设备端也排查 poll 预算）
- WebServer 活跃时 `esp_task_wdt_reset(707)` 刷屏（既有 bug，CrossPointWebServer.cpp 多处 esp_task_wdt_reset 与任务生命周期冲突）
- 设备 Wi-Fi 频繁掉线（HTTP 传输不稳定，需 wifi_prepare 恢复）
- 深度睡眠唤醒后 daemon 串口句柄失效（需 kill 死 daemon 重启唯一实例）

## 4. 硬件/操作纪律（铁律）

见仓库根 `AGENTS.md`，核心：
1. **m4adb daemon 全局只允许一个，活着就绝不动**（不 pkill/不重启/不删 socket）
2. 设备交互一律走 daemon socket，禁止 `--no-daemon` / 直接 serial
3. 只有 daemon 确认死亡才清理 socket 起新的
4. 刷写仅限 `murphy_m4_app1_flash.py`（APP1 only）+ otatool wrapper（`/tmp/m4-ota-bin/otatool.py`，因 otatool shebang 指向 anaconda python 缺 esptool）
5. 插件安装正确流程：`wifi_prepare` → `m4adb install`（install_http 秒级，勿用 USB 分片）

## 5. 测试方法

```bash
# 固件编译
python3 -m platformio run -e murphy_m4   # 注意先建 symlink 到 fengyan 仓库的 lib
# SDK 改动后需 rm -rf .pio/libdeps/murphy_m4/EInkDisplay 强制重拷

# 刷写（APP1 only + 切 slot 1）
IDF_PATH=.../framework-espidf python3 scripts/murphy_m4_app1_flash.py --port /dev/cu.usbmodem101 --firmware .pio/build/murphy_m4/firmware.bin --skip-backup --i-understand-app1-only
PATH=/tmp/m4-ota-bin:$PATH ... /tmp/m4-ota-bin/otatool.py --port /dev/cu.usbmodem101 switch_ota_partition --slot 1

# daemon
nohup python3 scripts/m4adb.py --port /dev/cu.usbmodem101 daemon --ready-timeout 90 &

# Waveform Lab 测试
python3 scripts/waveform_lab.py --port /dev/cu.usbmodem101 lut --hex "$(cat /tmp/lut_anim30.txt)"
python3 scripts/waveform_lab.py --port /dev/cu.usbmodem101 baseline /waveform/f_black.bin
python3 scripts/waveform_lab.py --port /dev/cu.usbmodem101 wipe /waveform/f_black.bin /waveform/f_white.bin --steps 6 --feather 12
```

帧文件（48000B raw）放 SD `/waveform/`，上传走设备 HTTP `/upload?path=/waveform`（需先 wifi_transfer 打开 + `/mkdir` 建目录，HTTP 不稳定需重试）。

## 6. 关键文件索引

| 文件 | 说明 |
|---|---|
| `src/debug/M4WaveformLab.cpp` | 实验服务核心（帧槽/LUT/合成） |
| `src/debug/M4SerialDebugBridge.cpp` | lut_* 命令 + chunk 上传 |
| `lib/hal/HalDisplay.cpp` | 透传层 |
| `open-m4-sdk/.../FreeInkDisplay.cpp` | waveformLabRefresh/Baseline/powerOnIfNeeded |
| `open-m4-sdk/.../Ssd1677Driver.cpp` | setCustomLut/refresh/displayWindow 实现 |
| `open-m4-sdk/.../Ssd1677Luts.h` | 出厂 LUT（grayscale/sticky/factory/x4pro_partial） |
| `scripts/waveform_lab.py` | 电脑端工具 |
| `scripts/page_turn_gen.py` / `page_wipe_gen.py` | 帧生成 |
| `/tmp/lut_anim30.txt` 等 | 实验 LUT 模板（TP 扫描系列） |

## 7. 参考（其他 AI 建议，已吸收）

- KOReader 分层：阅读器只发 `requestPageTurnAnimation(direction)`，显示后端决定步数/区域/波形
- 3~4 条带擦入（wipe）优于 12 帧全屏平移（避免同像素反复黑白驱动导致残影累积）
- 双 LUT：`PAGE_ANIM_STEP`（短快、允 80-90% 端点）+ `PAGE_ANIM_FINAL`（固化）+ 定期 `PAGE_CLEAN`
- 窗口条带：每步只写新增条带 → 减少 SPI 带宽 + 渐进视觉
- Good Display GDEQ0426T82 官方 LUT 示例包（`A32-GDEQ0426T82-LUT`）可作为波形参考
