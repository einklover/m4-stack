# M4 子代理画像（按轮更新）

> 只写观察，不写广告。每轮合并后由协调者追加。用来决定下一轮 prompt 要写多细、文件范围收多紧。
> 启动配置以 Paseo `list_models` 为准：配置里没有 named profile，必须把 provider/model/mode/thinking 写进 `create_agent`。

## 启动配方（2026-08-31 实测）

`list_profiles` 返回空。不要猜 profile 名。

| 用户说法 | provider/model | modeId | thinkingOptionId | features |
|---|---|---|---|---|
| Luna Max | `codex/gpt-5.6-luna` | `full-access` | `max` | （无） |
| Muse Code Ultra | `muse-acp/muse-spark-1.2-contributor` | `build` | `ultra` | `auto_accept: true` |

不要用：`codex/gpt-5.6-sol`（硬禁）。Terra 本编排不用。

回调：`create_agent` / `send_agent_prompt` 保持 `notifyOnFinish: true`。`send_agent_prompt` 的 `running` 不够，必须再看 `get_agent_status` + `get_agent_activity` 的 `lastUserMessageAt`。

## Luna Max（`gpt-5.6-luna` · thinking max）

### 适合

- 生命周期、并发、UAF、cancelled 路径、内存边界
- 只读根因 + 最小补丁
- 指出「这是架构问题还是实现 bug」

### 不适合 / 边界

- 视觉像素、主题包、QEMU 截图对照：历史更偏分析，像素活交给 Muse
- thinking `max` 会慢；不要塞进「改三个文件的小 UI」
- Codex 默认审批流会卡住；本编排用 `full-access`，prompt 里仍禁止 GitHub / 真机 / reset
- 容易把范围扩成重写；prompt 必须列出允许的文件和「不要重写 Scene 框架」

### prompt 要写到什么细

- 工作区绝对路径、允许/禁止文件
- 禁止事项（pio 并发、hostfwd、Sol、origin push）
- 验收命令
- 「找不到实锤就写报告，不要为了有 diff 而改」

### 本编排观察

| 轮 | 任务 | 遵守范围 | 速度 | 备注 |
|---|---|---|---|---|
| 0 | 启动 | n/a | n/a | `create_agent` 后 `lastUserMessageAt` 空了约 15s，activity 也空；必须再查一次才算真开始。thinking `max` + `full-access` 已生效。 |
| 1 | Scene 取消解码 / UAF | 遵守：只动 decoder + lifecycle 测试 + 报告，commit `7d46e6f` | 慢于两个 Muse，但交卷完整 | 最小补丁质量高（scratch 再 commit）。找不到实锤会写进报告而不是乱改不允许的 `HomeActivity`。RED 测试先失败再补丁。下一轮仍要写死文件白名单，否则仍可能扩成框架重写。Host 测命令必须逐个 flag 写出（zsh 不会拆 `$INC`）。 |
| 2 | AppList 宫格抽屉 | 遵守：只动允许的 4 个 src + 报告，commit `33b592e` | 慢于两个 Muse，交卷完整 | 产品路径对：宫格、内置+插件、plugin-only 卸载。没有写契约要求的字面量 `builtin.files`/`builtin.settings`（用 enum + `kFileManager`）。源码扫描测试会红；合入后由协调者加 canonical id 与 dock 路由。不要让 Luna 为扫描测试改 API 名。QEMU：「更多」进 `AppList`，插件图标正确；抽屉「文件管理」仍走 `UIIcon::Folder` 4 灰，不是 dock 的 1-bit 线稿。调用 `drawItemIcon(renderer, item, tile)` 与 2 参声明不一致——lane 禁 PIO，这种 API 对不上由协调者 host 编译修。 |

## Muse Code Ultra（`muse-spark-1.2-contributor` · thinking ultra）

### 适合

- Home/Scene 实现、封面路径、测试补丁、按文件清单改代码
- 视觉相关实现（仍须 QEMU 真 framebuffer，Preview 不算过）

### 不适合 / 边界

- 共享 worktree 时曾和别人抢同一文件 / 同一 `.pio`
- thinking `ultra` 仍会漏约束，除非 prompt 把路径和禁令写死
- ACP 权限提示会停；本编排 `auto_accept: true`
- 不要让它兼做协调者（合并、验收、写总谱）

### prompt 要写到什么细

- 与 Luna 同样的绝对路径 + 文件白名单
- 明确「只 commit 本 lane 分支」
- 测试用 `g++-14` 不是 Apple clang++（libc++ 缺 `__builtin_ctzg`）
- 结束必须写 `docs/orchestration/rounds/round-N-<lane>.md`

### 本编排观察

| 轮 | 任务 | 遵守范围 | 速度 | 备注 |
|---|---|---|---|---|
| 0 | 启动（impl + tests 各一） | n/a | n/a | 两个 Muse `create_agent` 后立刻有 `lastUserMessageAt` + `[Thought] Thinking...`。`auto_accept=true`、thinking `ultra` 已生效。 |
| 1 | tests-only generate-on-miss 契约 | 遵守：只动 4 个允许文件，commit `b6cc904`，不 push、不 pio | 快；交卷后独立复跑三套 host 测试都过 | 文件白名单有效。会把测试写得很长（+483 行 native）。mock convert 自己造 1-bit BMP，**不能**当成 Jpeg 转换器证据。last-resort 用字符串扫描 + skip，符合「A 未到就不要实现」。Python 继续堆 `assertIn` 源码片段；下一轮若要测转换器，必须在 prompt 里写死「链 JpegToBmpConverter，禁止 fake BMP 当 exact-size 证据」。prompt 仍要写绝对路径和禁令，否则容易扩到 src。 |
| 1 | impl last-resort 171×254→Scene 1-bit | 遵守：只动 cache `.h/.cpp` + round 报告，commit `7dbad34`；Jpeg/Home 未改 | 快；host mock + 快照 native/python 独立复跑过 | 文件白名单仍然有效。会把转换器写进 cache `.cpp` 而不是 Jpeg 库（允许集内，合理）。**不能改测试时会改生产来迁就快照**（64×64/50×80 探针硬编码）。自测 `/tmp/test_last_resort` 是它自己写的 mock，不是像素证据。下一轮 prompt 要写死：「不要为了旧测试绿而在生产里写探针尺寸；测试归 tests lane；真 BMP 转换必须有 host stub 或明确标未证」。 |
| 2 | tests dock/drawer contracts | 基本遵守：9 个新测试文件 + 报告 `be4f9f4`；**append** 了已有 `test_plugin_home_icon_resource.py`（加强锁，不是变绿） | 快；dock RED 在 A 合入后转绿 | 仍用源码扫描当契约。抽屉 RED 故意保留。`仅新建` 要在 prompt 里写成「禁止 git add 已有测试文件」，否则会 append。测不到 `HomeActivity` 发布陷阱（`draft_` vs published snapshot）也测不到 mockup 裁错行——host 绿 ≠ QEMU 图标对。 |
| 2 | impl dock icons + builtin.files | 遵守：7 个允许文件，commit `78f3573`；未动抽屉/`main.cpp`/测试 | 快；host 图标+dock 独立复跑过 | 内置图标做成 decoder 编译期数组，QEMU 不依赖 SD，合理。Dock 点击 `builtin.files` 仍走 `onGoToNativeApp`，会掉进应用列表——`main.cpp` 是 Luna 所有权，合入后由协调者补路由。**视觉裁切失败**：`mockup-icon-*.png` 裁的是最近阅读封面角 / 「全部」，不是 dock 行；黑像素约 2%，QEMU 像乱码。host 字节数/尺寸测试全绿。视觉任务必须在 prompt 写死：效果图 dock 行 y 范围、裁完黑像素约 15%、对照 mockup 四图标再 commit。 |

## 协调者（Grok / 本对话）

- 拆不重叠的文件所有权；越界 diff 整段打回
- 测和验收；host 先于 QEMU，QEMU 先于「看起来行」
- 每轮追加本文件，而不是只留在聊天里
- 原工作区 `m4-critical-ui-home` 保持用户脏状态，不在这里 `reset`/`clean`/自动 commit
- **只留给自己的活**：merge、canonical id 胶水、跨 lane 路由、`draftSnapshot` 这类 host 测不到的发布路径、QEMU 单会话（stop / detach / mcopy / 截图 / share-image）、小补丁（mkdir-exists、drawItemIcon 参数个数）
- Round 2 自评：合入与 host 测是对的；QEMU 验收把 `--keep-alive` + `--ready-seconds 120` 当成等待，USB 装 49KB 包，开机 4 次。视觉验收不要派子代理，但自己也不要空等。配方见 lessons：`--detach --ready-seconds 20`，图标在停机 `mcopy`

## 因材施教（按能力派活，不要平均拆）

观察来自 Round 1–2，不是广告。下一轮先按这张表派，再写文件白名单。

| 活 | 给谁 | 不要给谁 | prompt 里必须写死 |
|---|---|---|---|
| 按白名单改 Home/Scene/插件 BMP、decoder 绑定 | **Muse impl** | Luna（慢、会分析到想重写）；tests lane | 绝对路径；禁止测文件；禁止为旧快照硬编码探针；视觉则写 **裁切 bbox + 黑像素约 15% + 对照 mockup** |
| 宫格/路由/生命周期/UAF/并发、最小补丁 | **Luna Max** | Muse（会漏 cancelled/UAF，或用生产迁就测试） | 「找不到实锤就写报告」；禁止重写 Scene；不要为扫描测试改标识符字面量 |
| 新建 host/pytest 契约，允许 RED | **Muse tests** | impl（会改生产让旧测试绿）；Luna（会把测试写成审计论文） | 「禁止 `git add` 已有测试」；禁止 fake BMP 当转换器证据；不要用会碎的 `!exists` 极性扫描 |
| merge、id 胶水、QEMU、mcopy、截图 | **Grok 协调者** | 任何子代理 | 单 PTY / 单 SD；`--detach --ready-seconds 20`；`timeout: 0` 才允许 keep-alive |
| 像素对照、主题预览、图标是否像效果图 | Muse **出图** + Grok **QEMU 验收** | Luna（QEMU 抽屉功能对、系统图标丑也交差） | Muse 交 host 绿不够；协调者看 framebuffer |
| 真机 flash / APP0 / origin push | 人 | 所有模型 | — |

### 怎么写给谁听

- **Muse Ultra**：听话、快、白名单有效。短处是「测不过就改生产」和「视觉凭感觉裁」。把它当熟练施工队：步骤、bbox、禁令、验收命令逐条。不要让它兼协调。
- **Luna Max**：慢、完整、架构判断准。短处是标识符不跟扫描测试走、偶发 API 对不上（禁 PIO 所以自己编不过）、视觉用系统 glyph 交差。把它当外科医生：生命周期、路由、并发。产品行为用中文验收，字面量由协调者事后补。
- **两个 Muse 不要做同一类活**：一个 impl、一个 tests。否则抢文件或互相把契约写成对方实现。
- **不要用子代理加速 QEMU**。可并行的是开机前的裁图 / pack / host 测，且必须在第一次 `m4sim run` 之前做完。
- **小问题协调者直接修**（用户原话）。mkdir-exists、canonical id、`drawItemIcon` 参数、snapshot 拷贝、recrop，都不要再开一轮 Luna。大麻烦才开子代理：新 Scene 生命周期、新抽屉交互、新转换器。

### 下一轮建议拆法（若还做 Home 抛光）

| 若要做 | 模型 | 原因 |
|---|---|---|
| 抽屉「文件管理」改用 compiled 1-bit 文件夹 | Muse impl（只动 `AppListActivity` 图标绘制）或协调者 20 行 | 像素一致，不是架构 |
| 最近阅读封面空槽 | Luna（发布/绑定路径）或协调者先 host 复现 | 可能又是 snapshot/lifecycle，Muse 容易只改 decoder |
| 新主题/新图标 | Muse impl + 协调者 QEMU | 已证明 Muse 会裁错行，prompt 带 bbox |
| 加抽屉契约测试 | Muse tests 新建文件 | 字面量由协调者已写入生产后再测 |
