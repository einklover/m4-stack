# Murphy M4 AI 调试接口

这是 AI/自动化调试的首选入口。它只编排仓库已有工具，不连接真机，
不启动或维护 `m4adb`，也不会执行任何刷写。

## 30 秒上手

在 `murphy-m4-simulator` 目录运行：

```bash
# 查看等级（JSON，可直接给 AI 读取）
python3 tools/ai_debug.py --list --json

# 最快检查；默认就是 Level 0
python3 tools/ai_debug.py 0

# 运行单个等级
python3 tools/ai_debug.py 1
python3 tools/ai_debug.py 2 --scenario slow_sd_tap --seeds 1:1000
python3 tools/ai_debug.py 3
python3 tools/ai_debug.py 4

# 从 0 累进到目标等级
python3 tools/ai_debug.py 4 --through
```

每次运行的权威结果都在：

```text
build/ai-debug/summary.json
```

AI 应优先读取 `status`、`failed_step`、`steps[].tail`、`qemu_probe` 和
`artifacts`，不要从终端输出猜测成功与否。

## 模拟等级

| Level | 名称 | 首选场景 | 能证明 | 不能证明 |
|---:|---|---|---|---|
| 0 | contracts | JSON/工具/交接后快速体检 | 板级与 issue schema、Python 工具契约 | C++、固件运行 |
| 1 | model | reader、内存、存储、GPIO、SSD1677 | 确定性 C++ 模型和板级协议 | Xtensa/FreeRTOS |
| 2 | fuzz | 竞态、时序、偶发丢事件 | 多 seed 下的不变量 | ESP32-S3 真实执行 |
| 3 | qemu-boot | 启动、分区、PSRAM、编译器/运行时问题 | ROM、二阶段、Xtensa、FreeRTOS、`setup()` | 真实外设和模拟显示效果 |
| 4 | qemu-screen | UI、字体、旋转、framebuffer 提交 | 真实固件 UI 合成和 480×800 PBM | 触摸/按键、EPD 波形、鬼影、RF、电气 |

规则：先使用能真实复现问题的最低等级。低等级通过不等于高等级通过；
Level 4 通过也不等于真机显示效果通过。

## 常用参数

```bash
# 只生成计划，不执行
python3 tools/ai_debug.py 4 --dry-run --json

# stdout 只输出最终 JSON，适合 agent/tool 调用
python3 tools/ai_debug.py 1 --json

# Level 2：单场景和 seed 范围
python3 tools/ai_debug.py 2 --scenario tls_fragmented_heap --seeds 1:500

# 自定义固件仓库、产物目录或单步超时
python3 tools/ai_debug.py 4 \
  --firmware-dir ../wap-checkpoint/firmware \
  --output-dir /tmp/m4-ai-debug \
  --timeout 900
```

`--through` 表示运行 `0..LEVEL`；不加时只运行指定等级。日常定位应运行单层，
发布前门禁再使用 `--through`。

## 输出契约

`summary.json` 的稳定顶层字段：

```json
{
  "schema_version": 1,
  "status": "pass | fail | dry_run",
  "requested": {"level": 4, "through": false},
  "levels": [],
  "steps": [
    {
      "name": "qemu-screen",
      "status": "pass",
      "returncode": 0,
      "duration_s": 12.4,
      "command": [],
      "cwd": "...",
      "log": ".../qemu-screen.log",
      "tail": []
    }
  ],
  "artifacts": {},
  "qemu_probe": {},
  "screen": {"format": "PBM P4", "width": 480, "height": 800, "black_pixels": 2815},
  "next": "..."
}
```

失败时还会提供 `failed_step`，接口退出码为非零。所有子命令完整输出保存在
对应 `steps[].log`；`tail` 只用于快速判断，不能替代完整日志。

Level 3/4 的主要产物：

```text
build/ai-debug/murphy-qemu.bin
build/ai-debug/qemu-serial.log
build/ai-debug/qemu-serial.log.probe.json
build/ai-debug/qemu-screen.pbm       # Level 4
```

## AI 故障处理协议

1. 运行最低匹配等级；不要一上来就跑 QEMU。
2. 若失败，读取 `failed_step` 和该 step 的完整 `log`。
3. 在同一等级写最小回归并修复，然后重跑同一命令。
4. 只有问题涉及更高层时才升级；`summary.next` 会给出建议。
5. 涉及墨水屏外观、RF、电池或信号完整性时，停止模拟并标记
   `device_trace_required`，交给真机验收。

### 选级示例

- 页面跳转丢失、BUSY 竞态：Level 2，指定相关 scenario。
- SSD1677 命令/RAM/激活顺序：Level 1 的板级测试。
- PSRAM 分配、构造器、启动挂死：Level 3。
- UI 文字、方向、首帧：Level 4，并检查 PBM。
- 插件/provider JSON：使用插件 host simulator；不要硬塞进 M4Sim。
- 鬼影、LUT 灰阶观感：真机，任何模拟等级都不能下结论。

## 安全边界

这个入口永远不应包含以下操作：

- `m4adb` daemon 管理或串口占用；
- APP0/APP1、bootloader、分区表或整片 Flash 写入；
- 真机 reset、DTR/RTS、USB 串口探测；
- 把 QEMU 专用 `murphy_m4_qemu` 固件刷入真机。

真机操作继续严格遵守工作区根目录 `AGENTS.md` 的单 daemon、APP1-only
规则，并应由用户明确进入真机验证阶段。
