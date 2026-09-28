# 推箱子 QEMU 插件启动记录 2026-09-28

工作区 `/private/tmp/m4-sokoban-simulator-test-20260928`，分支 `task/m4-sokoban-simulator-test-20260928`，HEAD `3e3e096c4076da23f0b3b6328f42659087d776e1`。这次记录只包含实际跑过的命令和落盘日志。固件源码和插件包没有改。

## 环境

| 项 | 值 |
| --- | --- |
| PlatformIO | `/Users/zhouxinlai/.platformio/penv/bin/pio` 存在，Core 6.1.19 |
| QEMU | `/Volumes/z/paseo/migrated-home/cache/murphy-m4/espressif-qemu-v3/build-murphy-v3/qemu-system-xtensa` |
| QEMU 版本 | `QEMU emulator version 9.2.2 (v9.2.2-126-gfebae182e1-dirty)`，机器 `murphy-m4` |
| 会话目录 | `M4_PLUGIN_DEBUG_TMP=/tmp/m4-sokoban-qa-20260928` |
| 插件 | `com.m4.sokoban` 0.2.0 / versionCode 2，入口 `main.lua` |
| Lua host | 既有二进制 `/tmp/sokoban_lua_host`（243952 字节，Sep 27 13:54）。本次没有重编 |

`simulator/qemu/run_plugin_debug.py` 的 `build_firmware()` 会组装带 `~/.platformio/penv/bin` 的 `env`，但 `run()` 调用 `subprocess.run` 时没有把这个 `env` 传进去。第一次因此找不到 `pio`。没有改脚本，只在第二次调用前把该目录放进当前 shell 的 `PATH`。

## 第一次：未启动

```bash
cd /private/tmp/m4-sokoban-simulator-test-20260928
export QEMU_XTENSA=/Volumes/z/paseo/migrated-home/cache/murphy-m4/espressif-qemu-v3/build-murphy-v3/qemu-system-xtensa
export M4_PLUGIN_DEBUG_TMP=/tmp/m4-sokoban-qa-20260928
python3 simulator/qemu/run_plugin_debug.py \
  --plugin-src /private/tmp/m4-sokoban-simulator-test-20260928/plugins/m4-lua-sokoban-plugin \
  --app-id com.m4.sokoban \
  --seconds 90 --fresh-sd --no-net-check
```

退出码 2，约 0.7 秒。日志 `/tmp/m4-sokoban-qa-20260928/run_plugin_debug.attempt1.log`：

```text
+ pio run -e murphy_m4_qemu_plugin
FAIL FileNotFoundError: [Errno 2] No such file or directory: 'pio'
EXIT:2
```

这次没有编译，也没有 ping / install / launch。

## 第二次：编译、启动、安装、拉起

同一条 `python3` 命令，额外：

```bash
export PATH="$HOME/.platformio/penv/bin:$PATH"
```

完整日志 `/tmp/m4-sokoban-qa-20260928/run_plugin_debug.log`（3903 行）。进程退出码 0，墙钟约 118 秒。

编译：

```text
========================= [SUCCESS] Took 99.07 seconds =========================
murphy_m4_qemu_plugin  SUCCESS   00:01:39.075
```

`firmware.bin`：5672000 字节，sha256 `237c5e6ddf01f22082ab7c43e4e34e46d65f59d78843d71f30518c01a7c2a834`。

合成 flash 时日志打印 sha256 `92caadb2eef0e37d71f278b69acc056ebdb564164d08b375fb200d97778e9a79`。QEMU 退出后同一路径 `/tmp/m4-sokoban-qa-20260928/artifacts/murphy-plugin-16m.bin` 为 16777216 字节，sha256 `edd1f4e025c608c57a08c16c4e646ad9a46550742134bed3beb1a3a54e141bfd`。该文件就是本次传给 QEMU 的 MTD 镜像。

QEMU 命令使用 `-machine murphy-m4`，帧文件 `-global driver=murphy-ssd1677,property=frame-file,value=/tmp/m4-sokoban-qa-20260928/artifacts/ssd1677-frame.pbm`。串口 `/dev/ttys235`。

m4adb ping（`/tmp/m4-sokoban-qa-20260928/artifacts/ping.json`），就绪 7.8 秒：

```json
{
  "op": "ping",
  "protocol": 1,
  "firmware": "202608187-murphy-m4-qemu-plugin",
  "activity": "Home",
  "active_app": "",
  "screen_w": 480,
  "screen_h": 800,
  "sd_ok": true,
  "reset_reason": 4
}
```

安装（`artifacts/install.log`）：`com_m4_sokoban-0199ddc81f74.m4x` 8975 字节，USB 18/18，`install_ok=True`。

```json
{
  "op": "install",
  "noop": false,
  "id": "com.m4.sokoban",
  "version": "0.2.0",
  "versionCode": 2,
  "transport": "staged"
}
```

启动（`artifacts/launch.log`）：

```json
{
  "op": "launch",
  "app_id": "com.m4.sokoban"
}
```

`qemu.log` 末行：`qemu-system-xtensa: terminating on signal 15 from pid 48016`。复查时没有残留的 `qemu-system-xtensa` 进程。脚本在 launch 返回后结束了 QEMU，没有再发 key、tap、ui 或 screenshot。

## 帧

原始帧 `/tmp/m4-sokoban-qa-20260928/artifacts/ssd1677-frame.pbm`：48011 字节，头 `P4\n800 480\n`，sha256 `9c0e93f67076b8ea7dd70df8646354e9ba4a65a82a436104e0bb63e5f1584d63`。800×480 共 384000 位，置位 1355，墨水行约 y=184–294。

同内容 PNG：`/tmp/m4-sokoban-qa-20260928/artifacts/qemu-sokoban-first-frame.png`，1897 字节，sha256 `4ec2b8060a1705e1410b5fa4c062b788e4874fa78dea85d3edc176b6328f5692`。仓库副本 `docs/tests/M4_SOKOBAN_SIMULATOR_20260928-frame.png`，哈希相同。24 小时临时链接：http://179.255.101.123/share/EFaU_x5MbIUSh14IaL5eqQ/M4_SOKOBAN_SIMULATOR_20260928-frame.png

画面上的字是「推箱子」和「正在启动...」。字在 800×480 控制器帧里横躺，固件 ping 报告的逻辑分辨率是 480×800。这是启动闪屏，不是棋盘、选关或过关画面。

## 本次 QEMU 没有覆盖的玩法

启动闪屏之后会话已经结束。下面这些没有在这次 QEMU 帧上操作或复核：中后段关卡、方向键、触摸方向箭头、点远处空地、逐步撤销、选关翻页、过关下一关、异常进度恢复。

## 主机 Lua 回归

在同一工作区执行，日志 `/tmp/m4-sokoban-qa-20260928/lua-host.log`。这是 `/tmp/sokoban_lua_host` 跑插件脚本，不是上面的 QEMU 帧。

```text
test_game.lua OK
GAME_EXIT:0
test_ui.lua OK rects=74 lines=18 reachable=15
UI_EXIT:0
```

`python3 plugins/m4-lua-sokoban-plugin/tools/level_audit.py --quality-v2` 退出码 0。摘要：`level_count` 24，`one_push_count` 1，箱子分布 1/2/3 关为 1/16/7，`quality_v2_unmet` 为 `[]`。关卡 21–24 的求解统计在同一日志里：21（20 推 / 41 步）、22（21 / 54）、23（21 / 89）、24（32 / 97）。
