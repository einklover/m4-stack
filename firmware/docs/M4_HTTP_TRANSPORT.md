# M4HttpTransport — root-cause architecture

> Elevator pitch：这不是「让 WeRead 不再 OOM」的一次性补丁，而是给 M4 建立一个
> **内存可预测（memory-predictable）的统一 HTTP/TLS 传输底座**。所有 provider、
> 未来的 NetworkTask、甚至 Lua Host 的请求都收敛到同一个传输上，谁都不再各自
> 持有 `HTTPClient` / `WiFiClientSecure` 的生命周期。

## 1. Goal（为什么做，而不是只修 WeRead）

WeRead 每章会发起 4 次 HTTPS 请求（reader → e_0 → t_0/t_1 或 e_1/e_3），一次
handshake 需要 internal DRAM 里 **~40KB 连续块**。反复创建/销毁 mbedTLS 状态会
碎片化稀缺的 internal RAM，body 虽然已经流式到 SD/PSRAM，但 TLS 控制面仍会触发
OOM。

单看 WeRead，PR #10 的「pair reuse」（4→2 次 handshake）已经缓解了症状。但它是
一个**针对 URL 硬编码的 A/B 补丁**（`wereadReuseMode()` 里的魔法字符串），不是
根因方案。本设计的根因目标：

1. **一个传输，两种用**：`requestToSink`（短请求）与 `sessionBegin/End`（长会话），
   底层共用同一个持久 `esp_http_client` 句柄。
2. **provider 不再拥有传输生命周期**：providers 只描述「要什么」，传输负责
   TLS 生命周期、超时、单飞、流式 sink、堆预算检查。
3. **消灭每个 provider 各自的 `HTTPClient`/`WiFiClientSecure` 生命周期**：
   现状是 `M4NativeProviderHttp` 自己在 `perform()` 里维护 `HttpState`
   （`wereadSharedState()`）；目标是把这层收进传输内部，providers 不碰传输对象。
4. 为后续 **NetworkTask（网络常驻任务）** 和 **Lua Host 迁移** 铺路。

## 2. 分层图（providers → M4HttpTransport → esp_http_client → TLS → 内存域）

```text
 Lua Host / Native WeRead / Native JJWXC / Fanqie
                    │
                    ▼
         M4HttpTransport        ← 唯一的传输入口；持有一个持久 client handle
   (single-flight + NetworkTask(未来)；
    TLS 生命周期；timeout；stream sink；
    heap budget hooks；POD progress)
                    │
             esp_http_client
                    │            ← esp_crt_bundle_attach (见 M4NativeProviderLogin.cpp)
             esp-tls / mbedTLS
                    │
        ┌───────────┼───────────────┐
   internal DRAM    │              PSRAM / SD
   (Wi-Fi, lwIP,    │   (body RX/TX, JSON fragments,
    TLS control,    │    cookie, catalog, decode,
    DMA, RTOS)      │    Lua heap, FB)  —— PSRAM 优先 → SD
```

内存域（Memory domains）：

| 域 | 允许放什么 |
|----|-----------|
| Internal DRAM | Wi-Fi、lwIP、TLS 控制面、NetworkTask 固定栈、DMA、RTOS |
| PSRAM | body RX/TX、POST body、JSON 片段、cookie、目录、decode、Lua heap、framebuffer |
| SD | shard、combined、章正文、大的原始响应 |

## 3. Phase separation（阶段分离模型）——零 malloc 热路径

**TLS 激活期间**（body 流式写入时）不做任何会增长 internal 堆的事：

- 无 `std::string` 增长、无 `vector` 增长、无 `JsonDocument`
- 无第二个 TLS
- 无 font / framebuffer 分配
- 只有：**固定缓冲区 + `sink.write()` + POD progress**

**TLS 关闭之后**（连接释放）才允许 decode / JSON / UI 分配。

这是当前代码已经建立的纪律（`M4NativeProviderHttp.cpp` 的 `SinkStream` + `M4Psram`
流式窗口、`WereadProvider.cpp` 的 `DirectFileSink`），M4HttpTransport 把它固化为
契约而不是约定。参考 `M4xJsonStream::Sink`——body 永不物化，`Sink::write()` 直接
落盘。

## 4. Persistent NetworkTask（未来愿景）vs Phase 1 同步单飞

### 现状 / Phase 1：同步单飞

所有原生 HTTP 都跑在唯一的 native provider worker 上，并用
`M4NativeProviderHeavyGate::mutex()` 做**进程级单飞**：同一时刻只有一个 TLS/decode
任务。这是「保守但安全」的第一阶段——保证 internal RAM 竞争只来自一个 handshake。

Phase 1 就位的东西：

- `M4HttpTransport::requestToSink()`（同步、单飞、流式）
- `sessionBegin/sessionRequestToSink/sessionEnd`（可选会话；Phase 1 允许
  「每对复用」退化为「整章一个会话」之前的中间态）
- 全局递归锁（或复用 `M4NativeProviderHeavyGate`）
- HTTPS 前调 `tlsBlockAvailable()`，失败返回错误键 `tls_internal_oom`

### 未来愿景：持久的 NetworkTask

最终形态是**一个常驻网络任务**，TLS/传输状态长期存活，栈从 PSRAM 分配（固定大小，
见 `M4Psram::createTask` with caps），从而：

- 会话在 **章节之间** 也复用（不止 chapter 内的 pair），进一步减 handshake
- 请求可以异步排队、可中断、可取消
- providers / Lua 只是「提交请求 + 收流式结果」，完全不碰连接状态

> 里程碑：Phase 1 先做**同步单飞**（正确性优先）。NetworkTask 是同一传输的
> 调度器升级，不是新传输 —— API 不变，只换执行载体。

## 5. M4HttpTransport API 契约（供 Agent A 实现）

```cpp
namespace M4HttpTransport {
  struct Header { const char* name; const char* value; }; // 热路径不持有字符串
  static constexpr size_t kMaxHeaders = 8;
  struct Request {
    const char* method;          // "GET"/"POST"
    const char* url;
    Header headers[kMaxHeaders];
    size_t headerCount;
    const char* body; size_t bodyLen;
    size_t maxBytes;
    uint32_t timeoutMs;
    bool followRedirects;
    bool insecureTls;
  };
  struct Result {
    bool ok; int status; size_t bytes;
    char error[48];              // POD，不用 std::string
  };
  using ProgressFn = void (*)(void* ctx, size_t bytes);  // POD 回调
  using CancelFn   = bool (*)(void* ctx);

  // 单飞：body 流式写 sink，绝不累积。
  Result requestToSink(const Request& req, M4xJsonStream::Sink& sink,
                       ProgressFn progress, void* progressCtx,
                       CancelFn cancel, void* cancelCtx);

  // 可选会话：multi-request chapter 复用 esp_http_client 句柄。
  bool   sessionBegin(const char* hostHint);            // 如 "weread.qq.com"
  Result sessionRequestToSink(...);
  void   sessionEnd();

  void shutdown();                                      // 释放句柄
}
```

实现要点（Agent A）：

- 用 `esp_http_client` + `esp_crt_bundle_attach`（参考 `M4NativeProviderLogin.cpp`）
- 单个全局互斥锁（或复用 `M4NativeProviderHeavyGate`，是 recursive，外层作用域无害）
- HTTPS 前调 `tlsBlockAvailable()`；错误键 `tls_internal_oom`
- PSRAM 里的 RX chunk 缓冲（8–16KB）
- Progress 节流：~8KB / 250ms，回调只传 POD
- **不要用** Arduino `HTTPClient` / `WiFiClientSecure`

## 6. ALWAYSINTERNAL / RESERVE_INTERNAL —— 以后的实验（现在不要动）

当前 internal RAM 的稀缺本质上是：mbedTLS 的 handshake buffer **固定取 internal
RAM**，Arduino-ESP32 预编译包里没法把它挪进 PSRAM。缓解手段只能是「别的东西别来
争 internal」。

两个备选实验**明确推迟**，本 PR 一律不改：

| 实验 | 是什么 | 为什么推迟 |
|------|--------|-----------|
| `SPIRAM_MALLOC_ALWAYSINTERNAL` | 强制把 PSRAM 分配绕过/限制内部堆 | 改变全局分配策略，影响面不可控；需要平台层 + 全量回归 |
| `RESERVE_INTERNAL` | 预留固定 internal 块供 TLS 用 | 与现有「按需检查最大连续块」策略并存，需先验证预留量 |

> 硬规则：**不要在本 PR 改动 CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL / RESERVE_INTERNAL**。
> 这些是后续单独的实验 PR。

## 7. MBEDTLS_DYNAMIC_BUFFER / MEM_ALLOC_MODE —— 以后的实验

Arduino-ESP32 附带的 mbedTLS `framework-arduinoespressif32` 是预编译的，**没有开
DYNAMIC_BUFFER**，handshake buffer 固定占 internal RAM。要开 `MBEDTLS_DYNAMIC_BUFFER`
或调整 `MBEDTLS_MEM_ALLOC_MODE` 需要自编译 IDF/框架，超出当前平台构建链。

因此：

- **现在不做**：不重编 mbedTLS，不改 MEM_ALLOC_MODE。
- **以后实验**：若 build 链改为自编译，才评估用 `MBEDTLS_DYNAMIC_BUFFER` 让
  收发缓冲区握手期后回落，或 `MEM_ALLOC_MODE` 把部分 TLS 挪 PSRAM。
- 在此之前，M4HttpTransport 的「internal-RAM 竞争最小化」策略（单飞 + PSRAM-first）
  是唯一在预编译框架内可用的调度层手段。

## 8. Zero-malloc hot path + POD progress

热路径 = TLS 激活期间从 socket 到 sink 的字节搬运。目标 = **零 internal 分配**。

- 传输内部只用固定窗口（8–16KB PSRAM）+ 栈上 POD 状态机
- `ProgressFn` / `CancelFn` 是**裸函数指针 + void\* ctx**，不捕获、不构造 `std::function`
- progress 在 `sink.write` 内节流（见 `M4NativeProviderHttp.cpp` 的 SinkStream：
  8KB 或 250ms 才触发一次），避免每次 chunk 都 setPhase 造成的 mutex + string 抖动
- `Result`、`Header` 全部 POD，没有拥有字符串的成员

`M4xJsonStream::Sink` 是关键抽象：body 永不物化，chunk → sink → SD，内存有界。

## 9. WeRead：one-session-per-chapter（整章一个会话）

WeRead 每章 4 次请求。目标形态是 `sessionBegin("weread.qq.com")` 一个会话贯穿：
reader → e_0 → t_0/t_1（或 e_1/e_3），最终 `sessionEnd()` 拆连接，释放 TLS buffer。

- Phase 1 若整章会话太激进，允许**每对复用**（对应现有 PR #10 的 pair 边界）
  —— 但这是过渡态，最终收敛到整章一个会话。
- e_0 是 pair 边界（WereadProvider 需先读它判断 text/EPUB 模式），会话设计要尊重它。
- 现有 `DirectFileSink`（章正文直接落 SD）与 `PsvtsSink`（只扫 psvts）继续是
  session 的两种 sink。

## 10. Lua Host 迁移路径（later）

现状：Lua Host 有自己的一套 HTTP 处理，独立于原生 provider 路径。
目标：Lua Host 的网络请求最终也走 `M4HttpTransport`，从而分享同一套 TLS/内存纪律。

迁移路径（不阻塞本 PR，只是方向）：

1. 让 `M4HttpTransport` 暴露可被 Lua 绑定的薄接口（本质上一个 `requestToSink`
   风格的函数，输入说明 + 输出 sink / POD result）。
2. Lua Host 的请求从「自己管连接」改为「提交给 transport 拿流式结果」。
3. 这样 Lua 与原生 provider 共享：TLS 单飞、`tls_internal_oom` 检查、PSRAM-first、
   零 malloc 热路径。

> 本 PR 不做 Lua 改动，这里只记录长期路线，避免两边各维护一套 HTTP/TLS 栈。

## 11. 40KB gate → 从「架构」降级为「遥测」

`M4NativeProviderHeavyGate::tlsBlockAvailable()` 现在把 `kTlsHandshakeBlock = 40KB`
的「internal 里有没有 40KB 连续块」当作一个**准入阀门**：不够就返回
`tls_internal_oom`。

反思：40KB 是猜测值，未必精确等于真实 mbedTLS 需求。真正的根因是「internal RAM
稀缺 + mbedTLS 占用」，40KB 只是它的近似的监控代理。

- **目标**：随 transport 成熟，40KB 从「硬性架构门」退化为**遥测指标**——
  记录实际 handshake 前后的 internal 最大连续块/空闲量，用来校准真正需要的预留。
- **现在**：仍保留 gate 作为安全阀（保守，别在没把握时放开），但它不再是设计核心，
  而是被「单飞 + PSRAM-first + 会话复用」取代后的辅助护栏。

## 12. Phased rollout（分阶段上线）

1. **Core**（Agent A）：`M4HttpTransport.h/.cpp` 新文件，`requestToSink` +
   session + shutdown。提交到 `feat/m4-http-transport`。
2. **Docs**（Agent C，本文件）：架构定稿 + 更新 PLAN / NATIVE_TLS_MEM_COORDINATION 指针。
3. **WeRead**（Agent B）：`WereadProvider.cpp` 把 fetchChapter 走 transport；
   psvts + shard 用 `requestToSink`，章节优先 `sessionBegin`；transport off 时才回退
   `M4NativeProviderHttp`。
4. **TLS knobs**（单独实验 PR）：DYNAMIC_BUFFER / MEM_ALLOC_MODE / ALWAYSINTERNAL。
5. **POD progress 深化**：把残余 std::string 型 progress 字段清干净。
6. **Lua 迁移**：Lua Host 网络请求收敛到 transport。

每个阶段独立可验证，不互相阻塞；第 4–6 步明确不属于本 PR。

## 13. Merge / 分支约定

- 分支：`agent/m4-http-transport-core`（A）、`agent/m4-http-transport-weread`（B）、
  `agent/m4-http-transport-docs`（C，本分支）
- Merge 顺序：A core → `feat/m4-http-transport` → C docs → B weread →
  `pio build -e murphy_m4` 验证
- 硬规则：不改 APP0/分区；不 drive-by 重构；各自 commit + push 自己分支到 origin
