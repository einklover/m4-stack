# M4 多 worktree 编排（Grok 居中）

> 协调者：本对话（Grok）。子代理改代码；Grok 拆任务、合并、审计、测、验收。
> 回调：子代理 `notifyOnFinish=true`，完成/失败/要权限都会回到本对话，不要轮询 `list_agents`。
> 禁止：Codex Sol、GitHub push、刷真机、`git reset --hard` / `git clean`、并发全量 PlatformIO。

## 角色

| 角色 | 模型 | worktree | 分支 | 本轮文件所有权 |
|---|---|---|---|---|
| 协调者 | Grok（本对话） | `m4-critical-ui-home`（原脏树，不自动提交） | `feature/critical-ui-home` | 文档、合并、验收 |
| Lane A 实现 | Muse Code Ultra `muse-acp/muse-spark-1.2-contributor` thinking `ultra` | `m4-home-muse-impl` | `agent/home-muse-impl` | Home 封面生成路径与 JPEG 转换 |
| Lane B 审计/硬化 | Luna Max `codex/gpt-5.6-luna` thinking `max` | `m4-home-luna-audit` | `agent/home-luna-audit` | Scene 生命周期 / UAF / cancelled 路径 |
| Lane C 测试 | Muse Code Ultra 同上 | `m4-home-muse-tests` | `agent/home-muse-tests` | 仅 `firmware/tests/**` 与 `simulator/tests/**` |

路径前缀：`/Volumes/z/paseo/workspaces/paseo/worktrees/041rfr5o/`。

Paseo 工程：`prj_60a33d4e07df23cb`。原工作区：`wks_4bffa6a42500d80b`（M4 Critical UI Home）。

Git 对象库共用：`/Volumes/z/paseo/m4crosspoint/work/m4-stack/.git`。所谓「互相推送」是 **本地分支互 merge**，不是 `git push origin`。

## 一轮怎么走

```
Grok 写 Round N 任务书（文件所有权不重叠）
  → 三个子代理在各自 worktree 改代码
  → 各 lane 只在自己的 agent/home-* 分支上 commit
  → notifyOnFinish 回到本对话
  → Grok 审计 diff + 跑最小测试
  → 通过则 merge 进 agent/home-orch-integration
  → 各 lane `git merge agent/home-orch-integration` 拉齐
  → Grok 把本轮模型表现写入 docs/M4_AGENT_PROFILES.md
```

### 子代理 commit 规则

- 只 commit 本 lane 允许的文件。
- 信息：`round-N(<lane>): <一句话>`。
- 不要 commit `firmware/.pio`、`tmp-home-screenshots/`、`chatgpt.md`、凭据。
- 不要 `git push origin`、不要改 `feature/critical-ui-home`、不要动协调者工作区。
- 不要 `reset --hard` / `clean`。冲突停下来写 `docs/orchestration/rounds/round-N-<lane>-BLOCKED.md`，等 Grok。

### 协调者合并（本机，共享 git dir）

```bash
# 在任意 worktree 均可，因为对象库共用
git merge --no-ff agent/home-muse-impl -m "orch: merge muse-impl round N"
# 在 agent/home-orch-integration 上依次 merge 三路；冲突由 Grok 解，不丢给子代理互撕
```

拉齐：

```bash
git -C .../m4-home-muse-impl merge --no-edit agent/home-orch-integration
git -C .../m4-home-luna-audit merge --no-edit agent/home-orch-integration
git -C .../m4-home-muse-tests merge --no-edit agent/home-orch-integration
```

## Round 1 任务（文件不重叠）

已完成、不要重做：Home bind-time 从 `source.img` 生成 `cover_110x180.bmp` / `cover_74x106.bmp`；QEMU 已证明大/小封面有图。证据 `/tmp/m4sim-home-covers-fix.82YCwn`。

### Lane A · Muse Ultra · 实现

只改：

- `firmware/src/util/M4ProviderCoverCache.h`
- `firmware/src/util/M4ProviderCoverCache.cpp`
- `firmware/lib/JpegToBmpConverter/JpegToBmpConverter.h`
- `firmware/lib/JpegToBmpConverter/JpegToBmpConverter.cpp`
- 如必须：`firmware/src/activities/home/HomeActivity.cpp` / `.h`

任务：`source.img` 缺失时，才允许从同目录 `cover_171x254.bmp`（2-bit）生成 Scene 尺寸的 **1-bit 精确** `cover_{w}x{h}.bmp`。有 `source.img` 时禁止走 171×254。生成失败删残文件。不要 HTTP。不要在目录下载时预写两档。不要全量 `pio`。

自测：`/opt/homebrew/bin/g++-14 -std=c++17` 编 `firmware/tests/native_app/test_provider_cover_cache.cpp`（不要 Apple clang++）。Python：`python3 simulator/tests/test_provider_cover_cache_contract.py`。

结束时写 `docs/orchestration/rounds/round-1-muse-impl.md`（改了什么、命令、结果）。

### Lane B · Luna Max · 审计/硬化

只改：

- `firmware/src/util/HomeSceneRuntime.h`（及相关 `.cpp` 若已有）
- `firmware/src/activities/home/HomeSceneAssetDecoder.cpp` / `.h`
- `firmware/src/ui/**` 中与 cancelled / 生命周期相关的文件
- `firmware/tests/native_app/test_home_lifecycle_uaf.cpp`（可加断言，不要扩成大重构）

任务：审计 Home Scene 发布/取消/析构。若有真实 UAF、重复释放、cancelled 后仍写封面文件的问题，打最小补丁。不要重写 Scene 框架，不要改封面尺寸常量。不要全量 `pio`。

结束时写 `docs/orchestration/rounds/round-1-luna-audit.md`。

### Lane C · Muse Ultra · 测试

只改：

- `firmware/tests/native_app/test_provider_cover_cache.cpp`
- `simulator/tests/test_provider_cover_cache_contract.py`
- 如必须新增：`firmware/tests/native_app/test_cover_last_resort.cpp`（新文件）

任务：补 generate-on-miss 契约（hit / 从 JPEG 生成 / 不 HTTP / 精确 1-bit 尺寸 / 取消语义）。Last-resort 测试若 API 还没有，用 `#ifdef` 或跳过，不要为了测试去改生产代码。

结束时写 `docs/orchestration/rounds/round-1-muse-tests.md`。

## 验收（只有 Grok 做）

1. `git log --oneline agent/home-muse-impl agent/home-luna-audit agent/home-muse-tests ^agent/home-orch-base`
2. 文件所有权有没有越界
3. 最小 host 测试（native cover-cache + python contract）
4. 需要视觉时才编 `murphy_m4_qemu_plugin`（`PLATFORMIO_HOME_DIR=/tmp/pio_home2`），且同一时刻只有一个全量 pio
5. 通过才 merge integration；失败把范围缩回去再发下一轮 prompt
6. 把本轮模型表现追加到 `docs/M4_AGENT_PROFILES.md`

## 硬约束（写进每个子代理 prompt）

- 工作区必须是上表里自己的 worktree，不要写回 `m4-critical-ui-home`
- 先读 `docs/M4_AGENT_LESSONS.md` 和 `docs/M4_ORCHESTRATION.md`
- Mac 本地；不要刷 APP0/bootloader/NVS/全片；APP1 也只有用户明确说才能刷
- 不要 `pkill -f m4adb.py`；不要占用 `:18080` hostfwd（mihomo 已占用）
- QEMU 只用 patched `qemu-system-xtensa`，`-nic user,model=open_eth`，私有 16MiB flash + SD 副本
- `rg` 可能不存在，用 `grep`
- 不要伪造测试结果
