# M4HttpTransport 分步调试

单独调试底座（不启微信阅读 UI），把章节获取拆成可手动执行的步骤，每步返回：

- `ok` / `error` / HTTP `status` / `bytes`
- `before` / `after` 内存快照（heap / internal free / largest internal / TLS gate）
- `detail`（body 前缀、sess 标志、psvts 长度等）
- 串口 `[M4Http]` / `[WRHTTP]` 行
- SD：`/apps_data/com.weread.client/logs/http_transport.log`

## 步骤对照（WeRead 章节）

| 步 | `http_probe` step | 对应章节路径 | 网络 |
|----|-------------------|--------------|------|
| 0 | `mem` | — | 无 |
| 1 | `debug_on` | 打开 Serial+SD 日志 | 无 |
| 2 | `session_begin` | `sessionBegin("weread.qq.com")` | 无（仅 client init） |
| 3 | `tls_get` | 首次 HTTPS 握手 | GET `https://weread.qq.com/` |
| 4 | `weread_psvts` | reader HTML → psvts | GET `/web/reader/…`（需 cookie） |
| 5 | `weread_e0` | POST e_0 | 需 psvts |
| 6 | `weread_t0`/`t1` 或 `e1`/`e3` | 正文分片 | 视 e0 前缀 |
| 7 | `session_end` / `shutdown` | 释放 TLS | 无 |

`tls_get --no-session` = oneshot `requestToSink`；默认 `session=true` 走会话复用。

## 主机命令

```bash
# 推荐：一键逐步跑底座（到 tls_get）
python3 scripts/http_transport_step_debug.py --port /dev/cu.usbmodem101 --base-only

# 先 oneshot 再 session，对比是否只 session 崩
python3 scripts/http_transport_step_debug.py --port /dev/cu.usbmodem101 --base-only --oneshot-first

# 完整 WeRead 形路径（设备上已登录、有 cookie）
python3 scripts/http_transport_step_debug.py --port /dev/cu.usbmodem101 \
  --book-id <id> --chapter-uid <uid>

# 单步
python3 scripts/m4adb.py http_probe mem
python3 scripts/m4adb.py http_probe debug_on
python3 scripts/m4adb.py http_probe session_begin
python3 scripts/m4adb.py http_probe tls_get
python3 scripts/m4adb.py http_probe tls_get --no-session
python3 scripts/m4adb.py http_probe weread_psvts --book-id … --chapter-uid …
python3 scripts/m4adb.py http_probe weread_e0 --book-id … --chapter-uid …
python3 scripts/m4adb.py http_probe session_end

# 拉 SD 日志
python3 scripts/m4adb.py sd_read apps_data/com.weread.client/logs/http_transport.log --max 400
```

## 判读

| 现象 | 含义 |
|------|------|
| `session_begin` ok，`tls_get` panic/fail | 握手/perform 路径（DNS、mbedTLS、栈） |
| oneshot ok、session fail | 会话复用 / 句柄状态 bug |
| 两者都 `tls_internal_oom` | internal 最大连续块 < 40KB |
| `weread_psvts` → `login_required` | 设备无 cookie，与底座无关 |
| `weread_e0` → `empty_content` / `pfx={}` | 协议/鉴权/参数，TLS 已通 |
| panic `provider_stage=0x41x` | 见 transport stage 表 |

### diagnosticStage（panic breadcrumb）

| stage | 位置 |
|-------|------|
| 0x400–402 | sessionBegin |
| 0x410–414 | session perform（413=perform 内） |
| 0x420–422 | oneshot requestToSink |
| 0x430 | sessionEnd |
| 0x310 | HeavyGate TLS 块检查 |

## 固件侧

- `src/apps/M4HttpTransport.{h,cpp}` — MemSnap、`setDebug`、逐步 Serial/SD
- `src/apps/M4HttpTransportProbe.{h,cpp}` — 分步实现
- `m4adb` op：`http_probe`
- 正常打开章节也会打 `[WRHTTP]` 与 SD 日志（`setDebug(true)` 在 fetchChapter）
