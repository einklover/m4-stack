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
| 0 | 启动 | n/a | n/a | 尚未交卷 |

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
| 0 | 启动（impl + tests 各一） | n/a | n/a | 尚未交卷 |

## 协调者（Grok / 本对话）

- 拆不重叠的文件所有权；越界 diff 整段打回
- 测和验收；host 先于 QEMU，QEMU 先于「看起来行」
- 每轮追加本文件，而不是只留在聊天里
- 原工作区 `m4-critical-ui-home` 保持用户脏状态，不在这里 `reset`/`clean`/自动 commit
