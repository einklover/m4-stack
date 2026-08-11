# M4 内容加载流水线

## 目标

Lua 与 Native provider 共用同一套用户可见语义：缓存命中立即打开；未命中立即显示等待页；网络和解码在后台串行执行；当前章首屏稳定后只预取下一章；到章节末尾时，下一章已就绪则直接切换，否则退出阅读器进入等待页。

这套设计复用现有三层，不新增第二套加载框架：

```text
Activity / Reader
        │  Foreground / Prefetch
        ▼
M4NativeProviderManager ── M4ContentProviderSession
        │                         │
        │ worker                  └─ Missing / Fetching / Ready / Error
        ▼
Provider Adapter ── M4HttpTransport ── PSRAM / SD cache
```

## 公共接口和职责

### 请求入口

```cpp
M4NativeProviderManager::requestChapter(
    providerId, bookId, index0,
    LoadIntent::Foreground | LoadIntent::Prefetch);
```

- `Foreground`：由用户触发，可以重试 `Error`，拥有等待页和取消按钮。
- `Prefetch`：只接受 `Missing`，失败后保持安静，不在阅读器里循环重试 TLS。
- `ensureChapter(..., bool)` 仅保留作旧调用兼容层。

### 状态通道

`M4ContentProviderSession::ChapterStatus` 是唯一可观察状态：

```text
Missing → Fetching → Ready
                   └→ Error
```

`M4NativeProvider::Progress` 提供阶段和字节数：`Resolving → Connecting → Receiving → Decoding → Writing → Ready`。等待页只消费这些 POD 状态，不参与网络生命周期。

### 数据层

- internal RAM：Wi-Fi、TLS、固定任务控制面。
- PSRAM：HTTP/解码临时缓冲。
- SD：分片、组合文件和最终不可变章节缓存。

## 用户流程

### 从目录打开

1. 查最终缓存；命中则直接创建阅读器。
2. 未命中先进入 `NativeProviderBookActivity::Loading` 并绘制等待页。
3. `Foreground` 请求进入唯一 provider worker。
4. 下载、校验、解码、原子提交最终文件。
5. 状态变 `Ready`，等待页自动打开阅读器。

### 章节末页翻下一章

1. 下一章 `Ready`：阅读器直接切换缓存，无额外页面。
2. `Missing/Fetching/Error`：发布目标章节索引，关闭当前阅读器，父 Activity 立即显示等待页并继续或重试该请求。
3. 不在旧阅读器中叠加网络、字体、索引和 framebuffer 峰值。

### 后台预取

当前章第一页和索引稳定后，阅读器对 N+1 发一次 `Prefetch`。成功后末页可直接切换；失败不弹窗、不循环重试，用户真正翻章时转为 `Foreground`。

## 加速策略

### 通用

- 最终缓存命中直接打开。
- 单 worker、单 TLS 飞行，避免并发峰值。
- 同一逻辑会话复用 `M4HttpTransport` client；正文直写 SD。
- 第一屏绘制优先于 N+1 预取。
- 错误不在后台自动整章重跑，避免重复握手和隐蔽崩溃。

### WeRead

- `psvts` 是书级小 token，持久化在 `cache/<bookId>/psvts.txt`；后续章节跳过约 0.5–1MB reader HTML。
- cache miss 时一找到 `psvts` 就停止 reader body；因连接被中途取消，POST 前重建 session。
- cached token 失效时由 e0 结果识别，清缓存并只刷新一次。
- e0、t0/t1 或 e1/e3 每次生成自己的签名 body，同一章复用 transport session。
- 分片全部到齐后再做全局 reverse-swap、base64 和 XHTML 清洗。

## “下载到第一页就显示”的边界

普通明文 provider 可以在 Adapter 未来发布一个不可变 `ReadablePreview`，等待页达到最小可分页字节后先打开 preview，后台完成后再原子切换最终文件。

WeRead 当前密文需要多分片组合并执行跨文件 reverse-swap，第一片不能安全解释成正文。因此本阶段不让阅读器读取仍在增长/变换的文件；它通过书级 token 复用和 N+1 预取缩短首屏等待。若未来做 preview，必须由 provider 明确声明 `CapabilityReadablePreview`，不能让通用 reader 猜测半成品是否可读。

## 验收标准

- 缓存章立即打开。
- 未缓存章在一次屏幕刷新内出现等待页。
- 末页翻章：缓存命中直接切；未命中进入等待页，不死点。
- 连续章节不会因后台 Error 自动重试而持续 TLS churn。
- WeRead 同一本书第二个未缓存章不再请求 reader HTML。
- 连续打开章节时 internal largest block 不随章节数持续下降。

