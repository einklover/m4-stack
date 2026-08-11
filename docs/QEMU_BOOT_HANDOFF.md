# Murphy M4 · ESP32-S3 QEMU 启动交接（2026-08-11）

> **已完成（2026-08-11）**：GDB 证明镜像实际已进入 app startup，静默挂点是
> ESP-IDF `adc_hw_calibration()` 构造器等待 QEMU 9.2.2 不会产生的 ADC
> calibration-done 位，并非 PSRAM。`murphy_m4_qemu` 现通过 linker wrap 仅在
> QEMU build 跳过 `adc_hal_self_calibration()`，同时把 Arduino `Serial` 从真机
> USB CDC 切到 QEMU UART0。3 次冷启动均出现 `[M4-RC1] setup() start` 和
> `[M4-PSRAM] ... total=8388608`；本文件后续的“当前卡住”文字保留为排查历史。

> 给接手 AI 的完整上下文。先读本文件，再读仓库根目录 `AGENTS.md`（协作铁律）。  
> **本任务与真机刷机/m4adb 可并行，但勿把 QEMU 专用镜像刷到真机。**

---

## 1. 一句话状态

目标是让 **Murphy 固件 16 MiB 镜像** 在 Espressif ESP32-S3 QEMU 上越过 ROM / 第二阶段，进入 app `setup()`（理想到 Home）。  
**当前状态：已稳定进入 `[M4-RC1] setup() start`，Quad PSRAM 初始化成功；下一边界是尚未建模的 SDMMC 外设超时。**

用户原话链路：「新模拟器能模拟 bin 吗 → 试试最新 bin → 没有模拟成功吗 → **你补上**」。

---

## 2. 仓库与路径（本工作区）

| 角色 | 路径 | 备注 |
|------|------|------|
| 固件（活跃） | `wap-checkpoint/firmware/` | 分支 `feat/m4-http-transport` + 本地 WIP（含 Legado 等，**未要求提交**） |
| 模拟器 | `murphy-m4-simulator/` | 分支 `agent/murphy-board-model`；新增 `qemu/run_murphy_bin.py`（未跟踪） |
| 根协作纪律 | `AGENTS.md` | m4adb 单 daemon、只刷 APP1 等 |
| 旧总交接 | `HANDOFF.md` | **2026-08-08 TTF/Native 话题，已过期，勿当本任务状态** |

QEMU 相关文件：

```text
murphy-m4-simulator/
  tools/murphy_flash_image.py     # 合成 16 MiB 镜像（app0+app1 mirror）
  qemu/run_murphy_bin.py          # 直接调 qemu-system-xtensa（无需 idf.py）★ 本轮新增
  qemu/run_full_flash.py          # 走 idf.py qemu（需完整 ESP-IDF）
  qemu/probe_boot.py              # 串口阶段分类
  qemu/README.md                  # 里程碑 Stage 0–5
  tests/test_qemu_*.py

wap-checkpoint/firmware/
  platformio.ini                  # [env:murphy_m4_qemu] ★ 本轮改
  .pio/build/murphy_m4_qemu/      # QEMU 专用产物
  .pio/build/murphy_m4/           # 真机产物（dio + octal/OPI）
```

**禁止**：把 `murphy_m4_qemu` 产物刷进真机 APP1。真机仍用 `murphy_m4` + `scripts/flash_app1_once.sh`。

---

## 3. 环境

```bash
# PlatformIO
export PATH="$HOME/.platformio/penv/bin:$PATH"
# 不要用 anaconda python3 跑 esptool/pio 依赖链

# Espressif QEMU（本机已装）
~/.espressif/tools/qemu-xtensa/esp_develop_9.2.2_20250817/qemu/bin/qemu-system-xtensa
# version: 9.2.2 (esp_develop_9.2.2_20250817)

# macOS 依赖（文档要求）
# brew install libgcrypt glib pixman sdl2 libslirp
```

标准命令链（应能复现「卡在 entry」）：

```bash
cd wap-checkpoint/firmware
pio run -e murphy_m4_qemu

cd ../../murphy-m4-simulator
python3 tools/murphy_flash_image.py \
  --build-dir ../wap-checkpoint/firmware/.pio/build/murphy_m4_qemu \
  -o /tmp/murphy-qemu.bin

python3 qemu/run_murphy_bin.py /tmp/murphy-qemu.bin \
  --seconds 40 --psram-mb 8 \
  --serial-file /tmp/murphy-qemu-serial.log --probe
```

串口典型停点：

```text
ESP-ROM:esp32s3-20210327
rst:0x1 (POWERON),boot:0x4 (SPI_FLASH_BOOT)
mode:DIO, clock div:1
load:0x3fce2820,len:0x...
entry 0x403c88b8
# ← 此后无 boot: / spiram / [M4-RC1]
```

probe：`highest_stage=rom`，`second_stage_bootloader_reached=False`（因无匹配字符串；实际 ROM 已把 BL 装进 IRAM 并跳 entry）。

---

## 4. 里程碑（qemu/README.md）

| Stage | 含义 | 状态 |
|-------|------|------|
| 0 | 16 MiB 镜像被 QEMU 接受 | ✅ |
| 1 | ROM + 第二阶段 entry | ✅ |
| 2 | app 启动 / `setup()` | ✅ |
| 3 | 板级 HAL stub/模型 | ⚠️ 已到触摸/供电/PSRAM，停在未建模 SDMMC |
| 4–5 | 显示/输入/SD/网络 | 未到 |

成功判据（`probe_boot.py` / 串口）：

- 至少：`[M4-RC1] setup() start` 或 `app_main` / `Arduino`
- 更好：`[M4-PSRAM]`、`BOOT_SUMMARY`、`[MAIN] home1`

---

## 5. 根因结论（已实验验证，勿重复踩坑）

### 5.1 真机 profile vs QEMU

| 项 | 真机 `murphy_m4` | QEMU `murphy_m4_qemu`（本轮） |
|----|------------------|------------------------------|
| board | `esp32-s3-devkitc1-n16r8` | 同 board，**覆盖 memory_type** |
| Flash 头 | DIO（PlatformIO 故意把 qio 也打成 DIO） | 同样 DIO 头（正常） |
| PSRAM | **Octal / OPI**（`qio_opi`） | **Quad / QSPI**（`qio_qspi`） |
| QEMU 标志 | 需 `is_octal=true` + `-m 8M` | 默认 **不要** `--octal`，只要 `-m 8M` |

PlatformIO builder 关键逻辑（勿再浪费时间「改成 QIO 头」当主方案）：

```python
# platforms/.../builder/main.py _get_board_flash_mode
if mode in ("qio", "qout"):
    return "dio"   # 镜像头永远 DIO；运行时再升 QIO
```

裸改镜像第 3 字节为 QIO **不重算 SHA256** → 启动后 app hash 失败刷屏。  
重算 hash 后 BL 可校验通过，但 **不能** 单独解决 PSRAM 挂死。

### 5.2 实验矩阵（2026-08-11）

| 配置 | 串口结果 |
|------|----------|
| 生产/OPI 固件 + `is_octal=true` + 合法大 app | **挂死 entry**（BL 无输出） |
| 同上 + app 故意坏 hash | BL 能跑：`Image hash failed` 循环（说明 entry 后代码能跑，挂在「加载合法大 app / PSRAM」） |
| OPI 固件 + **无** is_octal + `-m 8M` | `octal_psram: wrong PSRAM line mode` → bail out → 不进 setup |
| `qio_qspi` 固件 + **无** `-m`（无 PSRAM） | `quad_psram: not connected`（证明 BL/app 启动路径有输出） |
| `qio_qspi` 固件 + `-m 8M`（有 Quad PSRAM） | **又挂在 entry**（疑似 PSRAM 初始化「成功路径」内部挂死） |
| 改镜像头 QIO 不重 hash | 有时看起来「更远」，是 hash 失败短路，**不是** 真成功 |

**结论一句话**：Espressif QEMU 9.2.2 上，Murphy ~5 MiB app + PSRAM（尤其 octal）与第二阶段/启动路径组合不稳；无 PSRAM 能打印错误，有 PSRAM 反而不出声挂死。

### 5.3 文档参考

- https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-guides/tools/qemu.html  
- https://github.com/espressif/esp-toolchain-docs/blob/main/qemu/esp32s3/README.md  
  - PSRAM：`-m 2M|4M|8M|16M|32M`  
  - Octal：`-global driver=ssi_psram,property=is_octal,value=true`  
  - WDT：**`timer.esp32c3.timg`**（文档写明 S3 复用 C3 名，不是 s3）  
  - strap：`esp32s3.gpio strap_mode=0x04`

`run_murphy_bin.py` 默认已用 `timer.esp32c3.timg`；`--octal` 默认 **关**。

### 5.4 argparse 坑（已修）

`argparse.REMAINDER` 放在 positional 后会吞掉 `--seconds` 等，把它们丢给 QEMU → `invalid option`。  
已改为 `parse_known_args` + 仅 `--` 后透传。

---

## 6. 本轮已落地改动（未要求 commit/push）

### 6.1 `wap-checkpoint/firmware/platformio.ini`

```ini
[env:murphy_m4_qemu]
extends = m4_base
board_build.flash_mode = qio
board_build.psram_type = qspi
board_build.arduino.memory_type = qio_qspi
build_flags =
  ${m4_base.build_flags}
  -DCROSSPOINT_VERSION=\"...-murphy-m4-qemu\"
  -DENABLE_SERIAL_LOG
  -DLOG_LEVEL=2
  -DM4_NATIVE_HEAP_DIAGNOSTIC=1
  -DOMIT_FONTS=1
  -DM4_QEMU_BUILD=1
```

- 链接验证：`firmware.map` 中大量 `qio_qspi`（不是 `qio_opi`）。  
- `M4_QEMU_BUILD` 宏 **已定义，但代码里几乎尚未使用**——可作 stub 钩子。

### 6.2 `murphy-m4-simulator/qemu/run_murphy_bin.py`

- 不依赖 `idf.py`  
- 默认非 octal；`--octal` 显式打开  
- WDT：`timer.esp32c3.timg`  
- `--serial-file` + `--probe`  
- 找 qemu：PATH / `QEMU_XTENSA` / `~/.espressif/tools/qemu-xtensa/**`

### 6.3 同目录其它 WIP（非本 QEMU 任务，勿误提交）

固件 `feat/m4-http-transport` 还有 Legado 目录/访客 IP 发现、catalog sink 等大量改动；**与 QEMU 无关**。交接 QEMU 时不要强制打包提交整包 WIP。

---

## 7. 建议下一手（按优先级）

### P0 — 定位 `entry` 后挂在哪

```bash
# 起 QEMU 停在入口等 GDB
qemu-system-xtensa -nographic -machine esp32s3 -m 8M \
  -drive file=/tmp/murphy-qemu.bin,if=mtd,format=raw \
  -global driver=esp32s3.gpio,property=strap_mode,value=0x04 \
  -global driver=timer.esp32c3.timg,property=wdt_disable,value=true \
  -nic user,model=open_eth \
  -serial mon:stdio -s -S

# 另一终端（工具链 gdb）
xtensa-esp32s3-elf-gdb \
  wap-checkpoint/firmware/.pio/build/murphy_m4_qemu/firmware.elf \
  -ex 'target remote :1233'   # 注意 run_murphy_bin 用 3333；手写常用 1234
```

关注：

1. 第二阶段是否卡在 **PSRAM 探测/校准**  
2. 是否卡在 **校验/映射 ~4.8 MB app 镜像**  
3. `call_start_cpu0` / `esp_psram` / `cache` 相关死循环  

BL ELF 若只有 `bootloader.bin` 无 ELF，可用 app ELF + 断在 `call_start_cpu0` / `app_main`。

### P1 — 用 `M4_QEMU_BUILD` 做最小可启动路径

思路（**仅 QEMU env**）：

1. 启动早期若 PSRAM 失败：**不要 abort/死循环**，打日志后降级  
2. 或 QEMU 下 **完全关掉 SPIRAM 依赖** 的硬路径（帧缓冲可改 INTERNAL 小缓冲 / 跳过），先摸到 `[M4-RC1] setup() start`  
3. 再逐步加回 `-m 8M`  

在 `main.cpp` / BoardConfig / PSRAM probe 处搜 `BOARD_HAS_PSRAM`、`esp_psram`、`[M4-PSRAM]`。

### P2 — 对照「能跑的」小镜像

`murphy-m4-simulator/qemu/` 下有 ESP-IDF smoke 工程（`PageTurnCoordinator` + heap）。  
先确认 **小 app + `-m 8M` 无 is_octal** 是否打印 spiram OK；若 smoke 也挂 → QEMU/本机问题；若 smoke 过、Murphy 挂 → 应用体积/配置问题。

```bash
# 需 export ESP-IDF
cd murphy-m4-simulator/qemu && idf.py build && idf.py qemu monitor
```

### P3 — 生产 bin 诚实边界

向用户对齐预期：

- **「模拟最新生产 bin」**：当前 QEMU 9.2.2 + octal **做不到**完整启动（已复现挂死）。  
- **「模拟 Murphy 逻辑」**：应用 `murphy_m4_qemu`（Quad PSRAM）+ 修好 PSRAM/stub 后，可到 Stage 2–3；**二进制与真机不完全同一 hash**。  
- 板级 EPD/SD/触摸仍要 Stage 3+ 模型，到不了完整 Home 交互。

### P4 — 工程化（Stage 2 通了再做）

- 把 compose+run 写进 `murphy-m4-simulator` README / CI（已有 workflow 骨架 `qemu-real-firmware` 可对照）  
- `probe_boot.py` 增加 `entry 0x` 作为 bootloader_reached  
- 单测：`run_murphy_bin` argparse 不吞选项  

---

## 8. 分区与镜像布局（合成器默认）

```text
0x000000  bootloader
0x008000  partition table
0x00E000  otadata（可选）
0x010000  app0   (0x6D0000)
0x6E0000  app1   (0x6D0000)  ← 真机开发刷写槽
0xDB0000  spiffs
0xFF0000  coredump
FLASH = 16 MiB exact
```

无 base dump 时 `murphy_flash_image.py` **mirror** 同一 `firmware.bin` 到 app0+app1，避免 OTA 槽选错。

真机 APP1-only 偏移 `0x6e0000`；QEMU 全镜像与 APP1 刷写是两条路径。

---

## 9. 不要做的事

1. **不要**把 `murphy_m4_qemu` / 改过 flash 头的 bin 刷真机。  
2. **不要**为了「保险」反复 kill m4adb daemon（见 `AGENTS.md`）；本任务通常 **碰不到** 串口设备。  
3. **不要**再把「镜像头改成 QIO」当主修复（PlatformIO 故意 DIO；坏 hash 假进度）。  
4. **不要**假设 `is_octal=true` 在 9.2.2 上对 ~5 MB Murphy app 可用——已证伪。  
5. **不要**静默 commit/push 整包 `feat/m4-http-transport` WIP（用户未授权）。  
6. **不要**默认开子代理并行改同一工作区（`AGENTS.md`）。

---

## 10. 给接手 AI 的最小复现检查单

- [ ] `pio run -e murphy_m4_qemu` 成功  
- [ ] `firmware.map` 含 `qio_qspi`  
- [ ] 合成 `/tmp/murphy-qemu.bin` 大小 **16777216**  
- [ ] `run_murphy_bin.py` 无 `--octal` 时命令行 **没有** `is_octal`  
- [ ] 串口仍停在 `entry` → 从 §7 P0 GDB 或 P1 stub 继续  
- [ ] 若只验证工具链：无 `-m` 应看到 `quad_psram: not connected`  

---

## 11. 关键命令速查

```bash
# 构建 QEMU 固件
export PATH="$HOME/.platformio/penv/bin:$PATH"
cd /Volumes/z/paseo/m4crosspoint/wap-checkpoint/firmware
pio run -e murphy_m4_qemu

# 合成 + 跑 + probe
cd /Volumes/z/paseo/m4crosspoint/murphy-m4-simulator
python3 tools/murphy_flash_image.py \
  --build-dir ../wap-checkpoint/firmware/.pio/build/murphy_m4_qemu \
  -o /tmp/murphy-qemu.bin
python3 qemu/run_murphy_bin.py /tmp/murphy-qemu.bin \
  --seconds 40 --serial-file /tmp/mq-serial.log --probe

# 干净杀残留 QEMU（本机调试用）
ps -axo pid=,command= | awk '/qemu-system-xtensa/ && !/awk/ {print $1}' | xargs kill 2>/dev/null

# 镜像头模式（仅诊断）：byte[2] 0=QIO 2=DIO；期望生产/ PIO 为 2
xxd -l 4 -s 0 /tmp/murphy-qemu.bin
xxd -l 4 -s 0x10000 /tmp/murphy-qemu.bin
```

---

## 12. 交接摘要（可贴进 chat）

```text
任务：Murphy M4 固件在 Espressif QEMU 上启动（用户「你补上」）。
状态：16MiB 镜像可 boot ROM，卡在第二阶段 entry；PSRAM（octal 必挂 / quad+ -m 也挂）阻 Stage2。
已做：env murphy_m4_qemu (qio_qspi)、run_murphy_bin.py、实验矩阵、文档。
勿做：QEMU 镜像刷真机；裸改 QIO 头当修复；默认 is_octal。
下一步：GDB 定位 entry 后 PC；或 M4_QEMU_BUILD 降级/关 PSRAM 先摸到 setup()。
路径：docs/QEMU_BOOT_HANDOFF.md + AGENTS.md。
固件：wap-checkpoint/firmware (feat/m4-http-transport WIP)
模拟器：murphy-m4-simulator (run_murphy_bin.py 未跟踪)
QEMU：9.2.2 esp_develop_9.2.2_20250817
```

---

*写于 2026-08-11，承接 session：QEMU 补全 / PSRAM hang 排查。*
