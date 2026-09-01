# M4 Agent Lessons / 常见踩坑经验

> 目的：记录 M4 项目中子代理反复踩过的坑。以后每次调度、实现、验证前先读本文件；发现新的可复现问题后继续追加，而不是只留在对话里。
> 范围：当前 M4 Home / Scene / QEMU / PlatformIO / 视觉复刻工作区，以及可迁移到其他 M4 固件任务的经验。

## 0. 调度硬规则

- **协调者禁止自己施工多文件视觉/主题/换行。** 用户已定：theme 槽、Gfx wrap、几何 pytest、图标裁切这类活交给 Muse Code Ultra；Grok 只审 diff、补十来行绑定/路由胶水、host 复跑、一次 QEMU。不要把「小问题直接修」扩成 renderer + theme + 五套测试。审核不要变成自己重写。
- **即使要亲自上手，也先把 Muse 当脚手架，不要自己搜半天。** 两行书名那轮慢的是协调者在错误 cwd 里 grep / 读 renderer / 找几何测试，不是那 10 行胶水。Muse 改不好、diff 要打回、或胶水必须自己写时：复用已有 Muse（不要新开 noop），prompt 只问 path + symbol + ~10 lines of context，禁止它再改代码。拿到路径后再 `read_file` 动手。禁止用协调者全库搜索代替这一问。工作区写死 integration 绝对路径。**给 Muse / Luna 的 prompt 用英文**（定位、施工、打回都一样）；对真人用户用中文。
- **语言分流。** 用户说中文 → 当面对人，回复中文。用户说英文 → 当其他 agent 在远程调本对话，回复英文、按 agent 协议交卷（路径、命令、artifact URL）。不要把英文远程调用当成闲聊。
- **中文对话里图片不要图床。** 对真人：截图转 RGB PNG 后用对话内 markdown 直接显示（`![说明](本地png路径)`），不要 `share-image` / Catbox / Litterbox。英文远程调用才给可拉取的 URL。1-bit `m4adb` PNG 先转 RGB，否则对话里打不开。
- **不要使用 Codex Sol 子代理。** 当前 M4 工作流只使用用户明确允许的非 Sol 代理；视觉/常规实现优先 Muse，复杂架构/并发审计可用 Luna。派活按能力，不要平均拆：表在 `docs/M4_AGENT_PROFILES.md`「因材施教」。Muse=快施工（白名单+bbox）；Luna=外科（生命周期/路由，不要像素）；Grok=merge/QEMU/胶水。不要派子代理去点 QEMU。
- 多 worktree 编排（2026-08-31）：协调者留在 `m4-critical-ui-home`；三个子代理分别在 `m4-home-muse-impl` / `m4-home-luna-audit` / `m4-home-muse-tests`。流程见 `docs/M4_ORCHESTRATION.md`，模型画像见 `docs/M4_AGENT_PROFILES.md`。Git 互推是共享对象库上的本地 branch merge，禁止 `git push origin`。
- Luna Max = `codex/gpt-5.6-luna` + thinking `max` + `full-access`。Muse Code Ultra = `muse-acp/muse-spark-1.2-contributor` + thinking `ultra` + `auto_accept`。`list_profiles` 为空时不要猜 profile 名。
- 创建新 agent 前先看正在运行的同类任务；同一连续任务优先复用，但不要为了复用而复用。
- **`send_agent_prompt` / `create_agent` 返回 running 不代表任务真的开始了。** 下发后必须再查 `get_agent_status` + `get_agent_activity`，确认 `lastUserMessageAt/activeTurn/activity` 已进入新任务，再向用户说“已开始”。此前 Logo/字体任务就出现过“API 返回 running，但 activity 仍停留在旧任务”的情况。Luna Max 在 2026-08-31 编排启动时 `status=running` 但 `lastUserMessageAt=null`、activity 空，约 15s 后才出现 user prompt。Muse Ultra 几乎立刻有 activity。
- 不要创建 noop/验证专用重复 agent。能用当前 agent 或顶层终端验证时，不额外开 agent。
- 给子代理的 prompt 要明确：工作区、允许修改的文件/范围、禁止事项、验收命令、截图/上传要求、不能伪造的数据、不能提交/清理 dirty worktree。
- Muse Ultra 在不能改测试文件时，会把快照探针尺寸写进生产（Round 1：`ensureSizedCoverFromSource` 对 64×64/50×80 禁用 last-resort）。下一轮 prompt 必须写死「不要为了旧测试绿而在生产里硬编码探针尺寸」。
- Lane 之间用源码子串当契约会在合入时碎（Round 1 tests 要 `!backend.exists(source)`，impl 改成正向 `if (backend.exists(source))`）。合入由协调者改测试，不要让 impl 迁就字符串扫描。
- 真 BMP→1-bit 转换不能用 mock `convert` / fake BMP 当证据。要证转换器就链 `JpegToBmpConverter` 或 `bmpFileTo1BitBmpWithSize`，否则标「未证」。
- zsh 下 `INC="-std=c++17 -I foo"; $GXX $INC` 不会拆参数。host 编译命令把每个 `-I` 写开。
- 「仅新建测试文件」不够：Muse tests 会 **append 已有** `test_plugin_home_icon_resource.py`。加强锁可留；若要禁改，prompt 写「禁止 `git add` 已存在的测试文件」。
- 子代理还在跑时，不要把 `agent/home-orch-integration` merge 进它的 worktree。Round 2 只拉齐了已收工的 Muse A/C；Luna B 等 notify。
- 本机 `python3 -m pytest` 走 Homebrew 3.14，没有 pytest。用 `/opt/anaconda3/bin/pytest`。`/usr/bin/python3` 才有 PIL。
- Dock 加 `builtin.files` 不等于能打开文件管理：`onGoToNativeApp` 只认 M4x package id，未知 id 会掉进 `onGoToApps()`。Round 2 已在 integration `4a34f22` 把 `builtin.files` → 文件传输、`builtin.settings` → 设置。
- Luna 抽屉实现了产品行为但没用契约字面量 `builtin.files`。Native 扫描测试仍会红。合入后由协调者加 canonical id，不要打回重写。
- Home dock 四个空方块（含 compiled `builtin.files`）不是 decoder/SD 问题：`addApp` 写的是 `draft_`，`publishHomeSceneWithAssetsCtx` 却扫 `draftPublication().snapshot`，`publish()` 才拷贝 snapshot。合入后 `appCount` 一直是 0，assets 从未挂上。QEMU 标签对、图标全是 `drawRect` placeholder。修法：decode 前 `draftPub.snapshot = model.draftSnapshot()`。Native host 测不到这条，因为不跑 `HomeActivity` 发布路径。
- compiled 1-bit `kBuiltinFilesIcon` 与 packaged `icon_home.bmp` 一样是 BMP 约定（1=纸/白）。`draw1BitAsset` 是 1=黑墨。文件 decoder 的 1bpp 路径会 `isBlack=!bitSet`，compiled 路径不会，不反相就是近黑方块。
- `mockup-icon-*.png` 曾从 mockup 裁错区域（最近阅读封面角 / 「全部」），转 1-bit 后只剩几条线。QEMU 看起来像乱码而不是图标。必须从 `home-mockup.jpg` 的 dock 行（文件夹 / 微信气泡 / 番茄 / J）重裁。黑像素占比约 15% 才像线稿图标；2% 基本是裁错了。
- QEMU panel frame `ssd1677-frame.pbm` 是物理 800×480。逻辑竖屏要用 `/usr/bin/python3` + PIL `Image.ROTATE_270` 转 480×800。`ROTATE_90` 会上下颠倒。Homebrew python3.14 没有 PIL，`./m4sim screenshot foo.png` 只会写出 `foo.pbm`。
- `m4adb install --transport usb` 在 QEMU `--no-daemon` 下，49KB 的晋江包会在分片中途 `SerialException: write failed: [Errno 5]`，随后 QEMU 被 SIGTERM。WeRead/Fanqie 小包 USB 安装是成功的。更大包：先 `./m4sim stop`，再用 `mcopy -o -i murphy-sd.img plugin/icon_home.bmp ::/apps/<id>/icon_home.bmp` 写 FAT，再 `--skip-build` 重启。不要在 QEMU 开着 SD 时写镜像。
- App 抽屉里的「文件管理」走 `UIIcon::Folder` + `renderer.drawIcon`（4 灰抖动），不是 dock 用的 compiled 1-bit `icon_home`。插件项才会画 `icon_home.bmp`。这不是 decoder 失败。
- QEMU 视觉验收最大浪费不是缺子代理，而是 **同一条串口上反复整机重启 × `--ready-seconds 120`**。keep-alive 的 ping 通常第 2 次（约 10s）就 READY，但协调者用 `get_command_or_subagent_output(timeout=130s)` 把每次开机空等到满 120s。图标更新应在 **QEMU 停着时** `mcopy` 三个 `icon_home.bmp`，然后 **只 boot 一次** 再截 Home + 点「更多」。不要 USB 装 49KB 包、不要为每张图重启。子代理不能并行抢同一个 PTY / 同一张 SD；可并行的是 packing、host 测试、裁图，且必须在第一次开机前做完。
- `./m4sim run --keep-alive` 必须用 **后台 + `timeout: 0`**。工具默认 120s/180s 会 SIGTERM 整个 process group，QEMU 看起来像“自己挂了”。`--ready-seconds` 用 20 即可，不要 120。
- `--ready-seconds` 是 ping 重试的 **deadline**，READY 后立刻返回。skip-build plugin-debug 实测第 2 次 ping（约 10s）就 READY。协调者不要对 `--keep-alive` 做 `get_command_or_subagent_output(120s)`——那个进程本来就不会退出。Agent 配方：`./m4sim run --plugin-debug --skip-build --no-net --ready-seconds 20`（默认 `--detach`，READY 后返回，QEMU 继续跑）。只有人要盯着终端时才 `--keep-alive`。
- App 抽屉标签不要用 `M4UiText::truncated(..., tile.width-8)`。UI_10 下四个汉字宽于格子，像素截断会变成「文件管…」。产品规则是按 UTF-8 码点数：`utf8EllipsizeChars(label, 4)`，四个字显示全，超过四个字才加 `…`。Host 测 `firmware/tests/native_app/test_utf8_ellipsize.cpp`。
- **十来行固件 UI 修补不要开子代理，也不要串行停 QEMU。** 完整节奏见文末「2026-08-31 — 小改动：先 PIO，再停 QEMU」。目标：改完 → PIO 与 host 测重叠 → 一次 `--ready-seconds 20` 开机截图。实测抽屉四字省略约 8–10 分钟，一半是先停机再全量编、再在协调者树里搜 `AppListActivity`。
- **工具默认 cwd 是协调者 `m4-critical-ui-home`。** 每一条 `pio` / `g++` / `pytest` / `compile_home_theme` / `./m4sim` 都必须先 `cd` 到 integration（或当前真正跑 QEMU 的那棵树）。相对路径会编错树、改错 generated header、pytest 报 No such file。grep 工具的 workspace 也是协调者，搜 integration 文件要给绝对 path。

## 1. 共享工作区 / Git：最容易造成二次事故

- 当前 Paseo agents 共享同一个 worktree。**严禁 `git reset --hard`、`git clean`、无授权 commit/push/rebase**，也不要“清理看起来无关”的脏文件。
- 每次开始先 `git status --short`，明确哪些是已有 dirty work，自己的改动只做增量。
- 多 agent 同时跑 PlatformIO / 生成同一 header / 生成同一 pack 会互相覆盖或让 build 目录看起来“消失”。**同一个 worktree 不要并发全量 PlatformIO build。** 实现 agent 正在写时，顶层只跑轻量测试；等写入完成再做最终 build。
- generated header、theme pack、preview PNG 必须从当前 source 重新生成后再验，不要相信旧 build artifact。

## 2. PlatformIO：反复踩的固定坑

### 症状
- `pio` 不在 PATH。
- 默认 `~/.platformio` 指向迁移目录，sandbox 写 `platforms.lock` 报：
  `PermissionError: Operation not permitted: .../.platformio/platforms.lock`
- agent 会反复“找 pio / 看 build 目录 / 等 scons / 再试 -j1”，浪费大量时间。

### 已验证正确做法

```bash
cd firmware
PLATFORMIO_HOME_DIR=/tmp/pio_home2 \
/Users/zhouxinlai/.platformio/penv/bin/pio run -e murphy_m4 -j1
```

QEMU：

```bash
cd firmware
PLATFORMIO_HOME_DIR=/tmp/pio_home2 \
/Users/zhouxinlai/.platformio/penv/bin/pio run -e murphy_m4_qemu -j1
```

如果 `PLATFORMIO_HOME_DIR` 路径不可用，再用已验证的隔离 core 方案，而不是反复尝试默认 HOME：

```bash
PLATFORMIO_CORE_DIR=/tmp/pio_core \
PLATFORMIO_BUILD_CACHE_DIR=$HOME/.cache/murphy-m4/platformio-build-cache \
/Users/zhouxinlai/.platformio/penv/bin/pio run --project-dir firmware -e murphy_m4 -j1
```

### 规则
- 首次失败如果是 `platforms.lock` 权限，**立即切 `/tmp` 隔离目录**，不要重复撞默认路径。
- 最终验收必须记录 exit code / SUCCESS / RAM / Flash；“build 进程存在”不算成功。
- 改完源码立刻 `pio run -e murphy_m4_qemu_plugin`。**不要先 `./m4sim stop`。** 正在跑的 QEMU 已经把上一份 firmware 载进 `flash-16m.bin`，编译 `firmware.bin` 不抢 PTY / SD。Host `g++` / pytest 与 PIO 并行。PIO SUCCESS 后再 stop + `--skip-build` 开机。
- 同一 worktree 不要并发两份 PIO。PIO 与 **已在跑的 QEMU** 不是同一类冲突。
- 若只改了 1–2 个 `.cpp` 却重编了 SdFat / FrameworkArduino：先看这次 `pio run` 是否重写了 `*Html.generated.h` / `center-kernel`（时间戳一变会打脏依赖图）。那是增量失败，不是省略号这类改动本身需要全量。

## 3. QEMU：Preview 绿 ≠ 真机/QEMU 绿

这是整个 Home 复刻过程中最常见的误判。

- Host preview 只能证明 compiler/scene 数据大致正确，**不能证明 GfxSceneRenderer 真正按同样语义画出来**。
- 曾出现：Host preview 正常，但 QEMU cover 视觉尺寸错误。根因是 `GfxSceneRenderer::draw1BitAsset(...)` 忽略目标 `rect.w/h`，按素材原始尺寸画；最终必须实现 cover aspect-fill + centered crop + rounded clip 才解决。
- 曾出现：QEMU App icon 乱点/噪声。根因不是布局，而是 renderer 用 `dummy[4]` 调 `drawIcon()`，下游按整张 bitmap 读取，造成越界读取。
- 曾出现：QEMU 没有真实 cover/icon，但 Preview 有。根因是 QEMU fixture 传空 cover / 空 `UiSceneAssets` 或程序化假图，不是 geometry 本身。

### 每次视觉改动的正确验证顺序
1. focused unit/native test；
2. compiler/theme tests；
3. fresh QEMU build；
4. fresh QEMU framebuffer；
5. 与**同一张权威 target**做 pixel/overlay 对比；
6. 最后才下“视觉已修复”结论。

不要只拿 Scene preview 给用户当最终效果。

## 4. QEMU fixture 必须区分“尺寸契约”和“真实像素”

- QEMU fixture 可以使用代表性数据，但如果用户要判断**图标/封面实际视觉是否一致**，就必须让 fixture 使用真实 production asset 或明确告诉用户“这里还是程序化占位图”。
- 之前 App icon 已经正式改成 **62×64**，plugin package 也包含从 target 精确裁出的 `icon_home.bmp`，但 QEMU fixture 一度仍使用程序化 icon，因此“尺寸已对齐”不能等价为“图案已对齐”。
- 验收时分开报告：
  - slot/bbox 是否一致；
  - asset 像素是否一致；
  - renderer semantics 是否一致。

## 5. 目标图必须唯一、固定、可校验

- 视觉复刻期间曾出现多个相似目标图、旧 overlay、旧 Scene preview，容易拿错图继续微调。
- 当前工作流必须在每轮视觉任务 prompt 里写清楚：**权威 target 的文件路径 / 尺寸 / hash（若有）**。
- 不要再使用旧的 overlay / 旧 target 做新一轮判断。
- 对 480×800 target，优先直接测 bbox / baseline / long-run 等像素指标，不要只凭肉眼。

## 6. 字体：字号值相同 ≠ 实际视觉相同

- Host preview 的 Pillow/系统字体，与设备 bitmap/system font 的实际字面尺寸、baseline、粗细可能不同。
- 不能只在 JSON 把 `ui_18_bold → ui_20_bold` 就宣称视觉完成；必须看 QEMU 实际 raster。
- 字体调整至少检查：
  - 字面黑像素 bbox；
  - baseline；
  - 与邻近元素间距；
  - 中英文 fallback；
  - weight 是否真的有变化。
- Logo 属于视觉资产/特殊字形时，不要默认普通 UI font 就能复刻；应单独处理或明确它只是近似。

## 7. Cover / Icon 尺寸合同：不要一处改、一处没改

- App icon 历史上存在 **68×68** 旧合同，而最终目标是 **62×64**。
- 修改尺寸时必须同步：
  - publication constants；
  - stride/arena bytes；
  - decoder exact-size validation；
  - QEMU fixture；
  - plugin packaged BMP；
  - tests。
- Cover rect 表示完整外框还是内容区必须明确。当前 cover rendering 已经踩过“rect 正确但 renderer 按 native size 画”的坑。

## 8. Template/bitmap 合成：白色不是总该覆盖

旧 `mofei-classic` 透明模板验证过一个关键语义：它是 **black-ink overlay**，不是 opaque background。

正确语义需要区分：
- 1-bit bit=1：画黑；
- bit=0：no-op / transparent；
- 不能把白色像素刷回去覆盖动态 cover/text。

在需要模板覆盖圆角边框时，顺序应是：动态内容 → 黑墨模板 overlay → transient focus。

后来统一 Scene 后，bitmap 节点的位置由 ordered node array 决定；**不要再引入 TemplateDraw / NativeDraw 两套模式。** 模板只是普通 bitmap node。

## 9. 不得伪造 Home 数据

反复讨论过但容易被 preview/demo 代码重新带回来：

- 没有真实 battery binding 时，不要显示假 `100%`。
- 不要把 wifi state 冒充 battery。
- `RecentBook` 只有可信 progress percent 时，只显示 percent；没有可信 `current/total chapter/page` 就不要假造 `120 / 336`。
- Preview 可以用 sample data帮助画图，但 production runtime 必须 unavailable → empty/hidden/fallback，不得生成假业务数据。

## 10. 图片：中文对话内嵌，英文远程才用 URL

对人（中文）：

- 逻辑 480×800 RGB PNG 后，回复里写 `![Home](path.png)`，让对话直接出图。
- **不要** `share-image`、Catbox、Litterbox。图床是给远程 agent 拉文件的，不是给人看的。
- `m4adb screenshot` 常为 1-bit；对话/read_file 打不开 mode 1。先 `/usr/bin/python3` + PIL 转 RGB。物理 800×480 PBM 用 `ROTATE_270` 再转。

对远程 agent（英文）：

- 给可拉取的 URL（`share-image` 或其它）。不要假设对方能渲染本机路径。
- `share-image` 曾 502，不要无限重试。上传失败先看服务，不要误判成截图没生成。

## 11. “验证代理”常见浪费模式

近期多个 agent 的 activity 里反复出现：
`Check build → Check again → Wait → Find cached firmware → Check scons state → Try without -j1`。

以后避免这种无边界循环：
- 构建前先确定唯一命令和唯一 artifact path；
- 一次 build 失败先读**第一条根因错误**；
- 如果是已知 lock/sandbox，直接切 `/tmp`；
- 如果 build 成功但 artifact 不见，先确认是否另一个 agent 正在重建/清理 build dir；
- 不要连续几十次 polling 同一目录。

## 12. 测试必须先 RED，但不要“为了 TDD 改旧测试迎合实现”

- 新行为应新增 focused regression，先确认旧实现会失败。
- 修改已有 expected geometry 时，必须能解释 target/source-of-truth 为什么变化；不能因为实现改了就顺手把测试期望改成实现值。
- 每轮视觉 polish 要保留“锁定几何”保护测试，避免修字体时把 cover/progress/icon rect 搞坏。
- 最终至少跑 `git diff --check`。

## 13. Completion 标准：没有 fresh artifact 就不算完成

子代理经常会给出“tests green / scope done”，但用户最终关心的是实际 Home screenshot。

视觉任务完成至少需要：
- fresh source/test result；
- fresh build；
- fresh QEMU screenshot；
- target overlay/diff（如果是复刻任务）；
- 用户可访问的图片 URL。

如果缺最后两项，就报告“代码/测试完成，视觉验收未完成”，不要说整体完成。

## 14. 每次 M4 子代理 prompt 默认附带的短检查表

以后调度 M4 代理时，将下面内容直接附在 prompt 末尾：

1. Read `docs/M4_AGENT_LESSONS.md` before touching code.
2. Preserve the shared dirty worktree; no reset/clean/commit/push.
3. Confirm the authoritative target asset/path before visual edits.
4. Do focused RED→GREEN tests; do not weaken locked geometry tests to match code.
5. Do not use fake battery/page/chapter data in production.
6. For PlatformIO lock errors, use the known `/tmp` isolated PlatformIO home/core immediately.
7. Do not run concurrent full PlatformIO builds with another writer in this worktree.
8. For visual completion, produce a fresh QEMU framebuffer, not only host preview.
9. If fixture pixels are placeholders, say so explicitly; do not claim artwork parity.
10. Upload final screenshots to Catbox and return the direct URL.
11. Before reporting “started”, verify agent activity actually entered this task; before reporting “done”, verify fresh artifacts exist.
12. Append any newly discovered repeatable pitfall/fix to this file.

## 15. Maintenance rule

- 本文件不是一次性总结。**以后每次发现可复现的新坑，直接追加“症状 → 根因 → 一次性正确做法 → 验证方式”。**
- 如果某条经验被新架构淘汰，不删除历史，标注 `SUPERSEDED` 并写替代规则，防止旧 agent/旧分支继续走老路。
- 代理任务完成后，如果过程出现了新的基础设施/构建/渲染坑，经验文档更新应作为完成条件之一。

## 18. QEMU profile / app-size boundary

### 症状

- A fresh `murphy_m4_qemu` image is passed to `m4sim run`, which waits for
  m4adb until timeout and may produce no direct frame.
- A current `murphy_m4_qemu_plugin` image can stop after QEMU prints `Adding
  SPI flash device`; there is no ROM/application serial banner, m4adb reply, or
  framebuffer, while an older smaller plugin image boots.

### 根因

- `murphy_m4_qemu` is the screen-only QSPI profile. `M4_QEMU_BUILD` renders
  Home, emits `[M4-QEMU-FB]` over UART, and returns from `setup()` before the
  m4adb bridge is initialized. `m4sim.py`'s generic path instead expects an
  m4adb `ping` and unconditionally passes `ssi_psram.is_octal=true`.
- `murphy_m4_qemu_plugin` is the interactive octal/OPI profile. Patched QEMU
  v3 hangs while loading current valid apps above the observed roughly 5 MiB
  boundary with `is_octal=true`; the old known-good plugin app was 4,857,408
  bytes, while the current app was 5,560,016 bytes.

### 一次性正确做法 / 验证方式

Use `murphy_m4_qemu` with `simulator/qemu/run_murphy_bin.py` (default: no
`--octal`), then extract the UART `[M4-QEMU-FB]` stream with `--screen-file`.
Use `murphy_m4_qemu_plugin` only for m4adb journeys, and check app size before
attributing a pre-banner QEMU stall to Home code. In the verified case the
fresh QSPI image reached setup at ~1.3 s, emitted a complete 48,000-byte frame,
and printed `Home scene bridge ready` at ~1.8 s.

Also serialize full PlatformIO builds in a shared worktree: tool invocations
can return an intermediate session while their child build continues, so check
for an existing `pio`/`scons` process before starting another full profile
build.

## 16. QEMU profile 与 runner 必须匹配

### 症状
- `murphy_m4_qemu` 能成功编译，但用通用 `simulator/m4sim.py run` 启动时一直等不到 `m4adb ping`，最终超时，容易被误判为“Home 启动失败”。
- 旧的 `murphy_m4_qemu_plugin` 镜像却能正常进入 Home 并被 `m4adb` 识别。

### 根因
- `murphy_m4_qemu` 是偏屏幕/fixture 的 QEMU profile：它可以渲染 Home，但不会建立通用 runner 期望的完整 serial/m4adb bridge 生命周期。
- `simulator/m4sim.py run` 的默认 readiness 判据是 `m4adb ping`；因此把 screen-only profile 和这个 runner 配在一起，本质是 **profile/runner 错配**，不是 Home 业务代码本身崩溃。
- 做“完整 Home + m4adb + framebuffer”验收时，应使用具备 bridge/plugin 启动条件的 `murphy_m4_qemu_plugin` profile。

### 一次性正确做法
- 只做 renderer/screen fixture 测试：可以使用 `murphy_m4_qemu`，但不要用 `m4adb ping` 作为唯一成功判据。
- 做完整模拟器 Home 验收：优先构建 `murphy_m4_qemu_plugin`，再使用通用 `m4sim.py` / m4adb 流程。
- 如果某 profile 没有 bridge，验收应依据该 profile 自己的 serial/framebuffer 证据，而不是硬套通用 readiness。

### 验证前检查
1. 先看 `firmware/platformio.ini` 的 env 定义和 build flags。
2. 确认该 env 是否初始化 serial bridge / m4adb 所需路径。
3. 再决定 runner 的 readiness 判据。
4. 不要因为 `m4adb` 超时就直接宣布 Home/QEMU boot 失败。

## 19. 工具调用返回不等于 PlatformIO 子进程已结束

### 症状
- 代理发起一次 PlatformIO build 后，工具层已经返回中间结果，但实际 `pio/scons` 子进程仍在后台运行。
- 接着再次“重试 build”，最终出现多个 `pio run -e ...` 并发写同一个 `.pio/build/<env>`，导致 artifact 消失、目录变化、链接阶段互相干扰，随后代理又开始反复轮询 build 目录。

### 根因
- 把工具调用返回误认为底层构建进程已经结束。
- 没有在重试前检查现存 `pio/scons/xtensa` 进程。

### 一次性正确做法
- 每次重新启动全量 PlatformIO build 前，先检查是否已有同 env 的 `pio/scons` 进程仍在运行。
- 同一个共享 worktree、同一个 env，任何时刻只允许一个全量 build 写 `.pio/build/<env>`。
- 如果发现重复 build，只停止自己这一轮产生的明确 PID；不要粗暴杀掉所有无关进程。
- 最终只接受一个串行 build 的完整 exit code / `SUCCESS` 作为验收证据。

### 经验更新
- “工具返回”与“构建结束”是两个独立状态。
- 在共享工作区里，重试之前必须做一次进程确认；这比连续 `Check build / Check again / Wait` 更可靠。

## 20. QEMU framebuffer orientation / PBM source

### 症状
- 旧 `m4sim` 的 `ssd1677-frame.pbm` 是 800×480（需 ROTATE_90 + FLIP），而 Luna High 已验证的 QSPI 正确流程 `simulator/qemu/run_murphy_bin.py` 产生的 `/tmp/m4-current-tree-home-qemu-runner.pbm` 已是 480×800（直接为正确方向）。

### 根因
- 两种 runner 使用不同 SSD 驱动/frame-file 路径，图像方向语义不同。

### 一次性正确做法
- 转换前先 `file`/`Image.size` 确认 PBM 尺寸；480×800 直接转 PNG，800×480 再旋转；不要假设所有 PBM 都是 800×480。

## 21. Authoritative target must be semantically validated (Home vs Settings)

### 症状
- 上一轮 overlay 误用 `tmp-home-screenshots/effect-target-480x800.png`（Settings 页效果图，hash `4cf1b950`）与 QEMU Home 做对比，导致 overlay/diff 看似“已对齐”实则语义错配。该文件与 `themes/mofei-classic/assets/home_reference.jpeg` 毫无关联。

### 根因
- 工作区同时存在多张 480×800 效果图（`effect480.png`、`effect-target-480x800.png`、旧 preview、旧 qemu 对比图），仅凭文件名或“480×800”尺寸无法区分 Home/Settings。

### 一次性正确做法
- Home 权威 target 必须同时满足：`Murphy M4` header 居顶、hero 阅读卡在 `44,104,138,191`、文案 `最近阅读` / `应用` 分别在 `y≈347/601`、且无底部导航；并与 `home_reference.jpeg` 经 Lanczos 缩至 480×800 后的像素一致（`effect480.png` 的 `md5_top 797d36ec` 与 `home_reference.jpeg` 重采样结果完全一致，而 `effect-target` 为 `4cf1b950` 完全不匹配）。
- 每次 overlay 前先做上述 bbox/文案/无底导的语义检查，并记录 `path + size + md5_top` 三元组；Settings 图必须显式拒绝。

## 22. Framebuffer orientation must be landmark-validated before overlay

### 症状
- 上一轮 QEMU PNG 虽尺寸 480×800 正确，但被判定为垂直上下颠倒，导致 overlay 中 header 与 app 行错位。

### 根因
- `run_murphy_bin.py::write_portrait_pbm` 已将 800×480 物理帧做 CCW 旋转并输出 `P4 480 800`，但不同 runner（`ssd1677-frame.pbm` 800×480 vs `run_murphy_bin` 480×800）语义不同；若对 480×800 再做额外 `FLIP_TOP_BOTTOM`，header 就会从顶部（`top60 876`）翻到底部（`top60 0 / bottom60 876`），hero 从 `y84` 翻到 `y486`，与 Home 权威图完全错位。

### 一次性正确做法
- 任何 overlay 前先做地标校验：`Murphy M4` header 必须在顶区 `y0-60`（`top60>500 && bottom60<100`），app 行必须在底区 `y630-750`（`>1000` 黑像素），hero 卡必须在 `y84-314`。对 `/tmp/m4-current-tree-home-qemu-runner.pbm` 原始 PBM 直接 `convert("L")` 即满足（`top60 876 bottom60 0 hero 5749 apps 1436`），额外垂直翻转则 header 到底、hero 到 `y486`，判定为错误。先验地标，再定 `identity` vs `FLIP_TOP_BOTTOM`，不得猜测。

## 2026-08-30 — Historical interactive flash ≠ current Home visual oracle; real data + current renderer required

### 症状
- 使用历史可启动 flash `/tmp/m4sim-smoke-20260828-205252/artifacts/flash-16m.bin`（firmware ping `202608187-murphy_m4_qemu_plugin`, full flash SHA `3cb3c192a8f2325a71bcc48bc9ccaded629699b9f49f92a29fdb8ef11dba3d0b` 16 MiB）成功走通交互 `m4adb`、Fanqie `com.fanqie.client` 安装/启动、catalog 24 本、detail 已持久化，但最终 Home 截图仅 5,661 黑像素、`y=52` 0，远低于当前 fresh QSPI Home 12,589 与权威 target 32,243（`y=52` 433-435），被判定为错误 renderer。

### 根因与一次性正确做法

1. **Historical flash ≠ current visual oracle.** 历史 flash 虽可跑交互 plugin，但其 Home renderer 来自旧分支；在 `feature/critical-ui-home` 上 `real plugin data + old Home` 不能作为当前视觉依据。权威截图需同时满足 (a) 真实 production 数据路径（plugin/provider，无 fixture/seed/fake `100%`/`120/336`）且 (b) 当前待测 branch 的 renderer；任一侧陈旧即 diagnostic-only。

2. **Visual sanity gate before presenting.** 权威 target `tmp-home-screenshots/AUTHORITATIVE_HOME_TARGET_480x800.png` (`9c3442…` `e6af38…` 32,243 黑, `y=52` 435 longest 433) vs 当前 fresh QSPI Home 12,589 `y=52` 434 vs 历史 plugin Home 5,661 `y=52` 0。结构性 landmark 大幅失配即判定错误 build/renderer/orientation，非样式微调。

3. **Agent child process lifecycle.** Muse/Paseo agent shell 下启动的后台 QEMU 子进程可能在 turn 结束时被回收，turn 内启动成功不等于跨 turn 存活。使用专用 Paseo persistent terminal 前台运行 QEMU，并跨工具调用用 `ps` + fresh `m4adb ping` 验证存活。`screen` 在此沙盒不可靠 (`getpwuid() can't identify your account!`); `tmux`/`setsid` 亦不可用。Paseo 终端需分开发送命令文本与单独的 `ENTER`。

4. **Direct QEMU FIFO bootstrap.** ` -serial pipe:/base` 不会自动创建 `/base.in`/`/base.out`。正确顺序：`rm -f base.in base.out` → `mkfifo base.in` → `mkfifo base.out` → 启动 QEMU (`m4sim.py:362-373`)。以 FIFO 存在/类型 `p`/fresh mtime 作为启动校验；漏掉 `mkfifo` 会导致 `Could not open ... No such file or directory`（已复现）。

5. **Hostfwd vs guest outbound.** `m4sim.py` 默认 `hostfwd=tcp:127.0.0.1:18080-:80`/`18081` 在 macOS 上被 ControlCenter 占用导致 `Could not set up host forwarding rule`。Fanqie 仅需 guest outbound，经 `-nic user,model=open_eth` 无 hostfwd 即可获 guest IP `10.0.2.15` 并拉取目录。区分两者，outbound-only 测试可省略 hostfwd。

6. **m4adb install transport / sandbox HTTP.** 默认 `auto` 会起本地 `WifiFileServer` (`wifi_serve.py:146` `socket.bind` → `PermissionError: [Errno 1] Operation not permitted` in sandbox)。这是宿主限制，非 plugin/QEMU 失败。显式 `m4adb install --transport usb` 走串口分片 (`m4adb_lib/client.py:320-332` `if mode=="usb": install_usb`)，可经 `/tmp/m4sim-home-plugin-real/artifacts/m4uart.pipe` 成功安装 Fanqie。先查 CLI/help/source 再选 transport。

7. **Single m4adb client per FIFO.** 同一串口 FIFO 上避免并发 `m4adb` 客户端；install 长命令执行期间勿并行 `ping`/`ui`/`screenshot`，否则串口组帧干扰。观察期间仅做进程检查，待首个客户端退出后再用 m4adb 验证；`running` 不等于卡死。

8. **Freshness / stale state.** 严禁将 `15:56` 旧 `ping.json`/`state.json` 与 `16:16` 新 `flash-16m.bin`/`qemu.log` 混用。时间戳必须关联；新 flash + 旧 ping 不是启动证明。历史 flash 拷入新 run 目录时需校验 hash/进程身份，而非仅文件名。

9. **The apparent ~5 MiB interactive limit was disproved.** 当前约 5.56 MiB 的 `murphy_m4_qemu_plugin` 镜像可完整启动至 Home；此前“无 ROM/banner”不能作为镜像尺寸边界证据。实际故障发生在 Home 后台任务进入 SD/registry 加载链之后，见下方 UART/栈预算结论。历史镜像仍只能作交互诊断，不能替代当前分支验证。

10. **Key/Back stack.** `m4adb key home` 不支持 (`bad_key`)。已验证：`Loading` → `Back` 取消回 `Detail`；`Detail` → `Back` 回 plugin home (`rows 24`)；plugin home → `Back` 回系统 `Home`。一次只发一个 `Back` 并校验状态；`ping` 粗粒度，`ui` 用于细分状态但需避免并发。

11. **Loading/detail/progress semantics.** `NativeProviderBookActivity state=3` 为 `Loading`（`State {Detail=0, CatalogLoading=1, Toc=2, Loading=3, Login=4, Reader=5, Error=6}`），非 `Error`。Fanqie 已加载 24 本且 book `6838480082219043843` 的 detail 已通过 `RECENT_BOOKS.updateProviderBook()` 在 `loadBookDetail`/`pollDetailLoading` 持久化到 Recent；章节拉取可独立停滞。未见 `TxtReaderActivity` 打开及 `M4ContentProviderSession::noteProgress` `lastOpenIndex0` 前，不得宣称已阅读/进度已记。

12. **Screenshot source/orientation/profile rules.** 交互 `murphy_m4_qemu_plugin` 原始 QEMU `ssd1677-frame.pbm` 为物理 `800×480`，直转 `PBM->PNG` 会得到误导性 landscape (`5661` 黑)。`m4adb screenshot` 为权威逻辑路径，直接导出逻辑 `480×800` (`P4 480 800`, `write_p4` 直写 `w/h`，host 不做几何变换)。已验证物理→逻辑映射 `src_x=y, src_y=479-x` (`m4_screen_viewer.py:99`, `run_murphy_bin.py:97`, `test_m4_screen_viewer.py:15`/`test_qemu_murphy_runner.py:9`)，不可目测猜测。Screen-only `run_murphy_bin` 的 480×800 帧可用 identity；方向与 runner/profile 相关，转换前先 `file`/`Image.size` 校验。

13. **Artifact validation/upload.** 截图本地 `dimensions`/`source`/`orientation`/`sanity` 通过后再上传；上传前重算本地 SHA。`https://files.catbox.moe/sgb703.png` 为旧 renderer 诊断产物，已显式标记 `INVALID/DIAGNOSTIC`，永不作为当前 Home 证据复用；本地预上传 SHA 仅证发送文件，远程建议另做下载+SHA 校验。

14. **Debugging discipline.** 按层分离诊断：build/profile 启动 → 进程持久化 → FIFO/serial → guest 网络 → plugin 安装 → 数据链路 → renderer 版本 → 截图方向 → 上传。每层成功不代表后层成功；结构性错误截图应立即作废而非因联网/数据成功而合理化；高风险 simulator 交互一次只做一个状态迁移并校验。

## 2026-08-30 — Empty m4adb ping is not Home-ready; capture UART before blaming QEMU

### 症状

- `m4sim run` 偶尔会把包含 `protocol`/`firmware` 键、但
  `firmware=""`、`activity=""`、`screen_w=0`、heap/PSRAM 全为 0 的早期
  `ping` 当作 READY；随后 UI 仍为空或固件重启，看起来像模拟器死机。
- 当前 `feature/critical-ui-home` dirty build 在完成 PSRAM、SD、display、font、
  open_eth 初始化并进入 Home 后，第一帧随即反复 `LoadProhibited` reboot。

### 根因与验证

- 空字段 ping 只证明串口 debug bridge 已响应，不证明应用/Home 初始化完成。
  readiness 至少应要求非空 firmware、有效 screen dimensions，并在 Home 验收中
  等到非空 activity/有效 framebuffer。
- m4adb 会消费 PTY UART，QEMU stdout log 不含 guest serial。需要定位早期 boot/panic
  时，用同一 QEMU 参数把 UART0 直接送到 `-serial file:<path>`，短时前台运行后再用
  当前 ELF 解码 backtrace。
- 初次 UART 证据为 `Guru Meditation Error: Core 1 panic'ed
  (LoadProhibited)`、`EXCVADDR=0x00000748`，backtrace
  `0x420bd04b -> 0x420bd222 -> 0x420bd2a1`；当前 ELF 的 addr2line 精确落到
  `HomeActivity.cpp:219` 的 `ctx->epoch.load(...)`，调用链为
  `sceneBackendTaskTrampoline -> backendLoop -> publishHomeSceneFromBackendCtx`。
  反汇编显示 shared_ptr 的 managed pointer 为 null，再访问 BackendContext 内约
  `0x748` 偏移的 epoch。该现象最终确认是任务栈破坏后的表象，而不是历史
  app-size boundary、SD/PSRAM 初始化或 QEMU loader 卡死。

### 最终根因、修复与复测方法

- `HomeSceneBackend` 的 8 KiB 栈不足以承载 `M4xRegistry::load`、SD、JSON 和资源发布
  的深调用链。使用独立 fresh SD 单实例复测时，它在两次 registry miss 后稳定触发
  `Double exception` 且回溯损坏；仅把该后台任务栈改为 16 KiB 后，同一镜像完成
  `BOOT_SUMMARY`、进入 Home，并连续完成两次 `GFX`/`M4-DISP` 提交，45 秒内无后续
  Guru Meditation 或 reboot。显示任务仍保持 8 KiB。
- 传给 FreeRTOS trampoline 的 `shared_ptr` holder 只移动一次；`xTaskCreate` 必须检查
  返回值并在失败时释放 holder。任务自删前还要显式 `reset()` task-local C++ owner，
  因为 `vTaskDelete(nullptr)` 不会执行普通 C++ 栈展开。
- 直接调试 QEMU 时，不要假设终端 Ctrl-C 已清理子进程。每轮前按明确 PID 检查并回收
  自己启动的实例，每轮使用独立 SD 文件；多个 QEMU 同写一张 SD 会制造二次异常并
  污染根因判断。推荐由带 timeout/finally 的前台父进程启动并回收 QEMU。
- **不要用 `pgrep -f qemu-system-xtensa`（或把该字符串写进同一条 bash 命令）。**
  Grok/Paseo wrapper 的 argv 含完整脚本文本，`-f` 会匹配 wrapper 自身并可能
  在 soak 开始前被工具层拦截/杀掉。查进程用 `pgrep -x qemu-system-xtensa`。
- **`set -e` + `diff -u` 会把“有差异”当成失败。** 隔离轮次的 diff-gate 用
  Python `filecmp` 列出差异文件，不要把 `diff` 的 exit 1 当构建失败。

## 2026-08-31 — Original Home StoreProhibited is not construction / trampoline / registry-on-backend

### 症状

脏树 `murphy_m4_qemu_plugin`（约 5.56 MiB）进入 Home 后仍出现原崩溃类
`StoreProhibited` `EXCVADDR/A2=0xfffffec0`（`vPortYieldFromInt`）。这与后来的
`LoadProhibited @ 0x748`（8 KiB backend 栈破坏、null `shared_ptr`）不是同一类。

### 在 `/tmp` 相对 `259cdbf7` 的隔离结论（dirty `HomeActivity.cpp` SHA 冻结
`d2dfef192f21f0611a038fde23a9b6123db8d18f63965a39620830296355d5d5`，未改脏树）

下列增量在 16 KiB `HomeSceneBackend` 上 45s Home soak **均 PASS**，无
`0xfffffec0` / Guru：

- R8：第二任务 noop
- R9：trampoline `unique_ptr<shared_ptr<R6BackendContext>>` 接管后挂起
- R10：backend `model.begin(Ready)` + `publish()`
- R11：backend 上 `M4xRegistry::load()` 后再 publish（UART 有
  `R11 backend registry load done`；free heap ~138668 vs R10 ~139360）

因此原 `0xfffffec0` **不是** BackendContext/model 构造、`publishLoading`、
创建第二 16 KiB 任务、trampoline 所有权转移、backend 上 registry load。

### 尚未纳入隔离树的脏树差异（下一刀优先）

R11 树仍无 `GfxSceneRenderer.cpp` / `HomeSceneAssetDecoder.cpp`；显示路径仍是
classic `render()`，且 `onEnter` 仍调用 `loadRecentBooks`。脏树 M4 路径跳过
enter 时 load，display 走 `renderSnapshotScene()`。下一刀应单独加这些，不要
整棵 dirty HomeActivity。

## 2026-08-31 — R12: GfxSceneRenderer on 8KiB display is not 0xfffffec0

### 编译坑

`murphy_default_m4theme.h` / `UiSceneTypes.h` 用 `#include <avr/pgmspace.h>`。
隔离 `murphy_m4_qemu_plugin` 第一次编 R12 在 `HomeActivity.cpp.o` 报
`avr/pgmspace.h: No such file`。Arduino 核没有这个路径；脏树用
`firmware/src/avr/pgmspace.h` shim（`PROGMEM` / `pgm_read_*` no-op）。
加 theme 时必须同时拷这个 shim 进 `/tmp` 隔离树。这是编译依赖，不是第二刀行为。

### 隔离结论

R12 树 `/tmp/m4-r12-snapshot-render.DH9txb`，run `/tmp/m4-r12-snapshot-render-run.iAcA7A`：
`murphy_m4_qemu_plugin` firmware.bin 5,212,048 B
`8edae0d5da557b8d3d94b2e0531ee6a5258dceb661583d0511405ab6492aa1c7`，
elf `217e9a33d5b28f7248f46c8316e6a8ff6bce60fcda90f9f84c72100ec757cbc8`。
45s SIGTERM wait rc 0。允许的 first-boot `startup_funcs.c:118` + 一次
`RTC_SW_CPU_RST`。随后 Home、`R11 backend registry load done`、两次
`R12 snapshot scene render done`（首帧 + backend `updateRequired`）、`[MEM]`
Free 138828 / min 137224 到 40s。无 Guru / `0xfffffec0`。SD 仍
`938a6da0…`。脏树 `HomeActivity.cpp` SHA 仍冻结 `d2dfef19…`。

因此 **8KiB display 上 `GfxSceneRenderer::render(murphy_default_m4theme, …)`
（空 recents、无 decoder、仅 publication→assets + PROGMEM theme blit）不是原
`StoreProhibited 0xfffffec0` 触发点。** 空 recents 的 enter-`loadRecentBooks`
在这张 SD 上也是空操作，不能当下一刀。

下一刀（仍 `/tmp`，不改脏 `HomeActivity.cpp`）：脏树即使 `recent.bin` 缺失仍会
`model.addApp` 并对 registry 插件跑 `HomeSceneAssetDecoder::decodeAppIconForPublication`。
优先单独加 decoder + app-icon decode，或先只把 registry apps 填进 model。

## 2026-08-31 — `StoreProhibited 0xfffffec0` 是 TCB-probe 固件，不是当前 Home 源码

R1–R24 在 `/tmp` 把脏树 `firmware/src` 逐步叠到 `259cdbf7` 隔离树后，
`murphy_m4_qemu_plugin` 45s Home soak **没有** 原类崩溃。R24 起
`firmware/src` 与脏树字节级相同；HomeActivity.cpp 仍冻结
`d2dfef192f21f0611a038fde23a9b6123db8d18f63965a39620830296355d5d5`，
源码中无 `TCB-PROBE`。

对照证据：

| 固件 | SHA256 / 大小 | UART | 45s Home |
|---|---|---|---|
| TCB-probe `/tmp/m4sim-tcb-probe` app `bfa1058778…` 5,560,368 B ELF `1cb9a8056` | Home `internal free` 后立刻 `[M4-TCB-PROBE]` 读 `pxCurrentTCBs` / `pxEndOfStack` | 随后 Core0 `StoreProhibited` `PC 0x40384096` `A2/EXCVADDR 0xfffffec0`，无 GFX。副本 soak `/tmp/m4-r26-tcb-repro-run.gisuVn` 可复现；崩溃后 QEMU 会 reboot 循环，SIGTERM 可能不够，需对 `qemu-system-xtensa` 精确 SIGKILL。 |
| 隔离 R24 `/tmp/m4-r24-remaining-src.4WdUTc` `d8513d7c…` | 无 TCB-PROBE；两次 GFX；`[MEM]` 到 40s | PASS |
| 脏树已有 `.pio` qemu_plugin `f42ee75c…` 5,559,904 B（只复制 soak，不在脏树重建） | 无 TCB-PROBE；两次 GFX；`[MEM]` 到 40s | PASS `/tmp/m4-r25-dirty-pio-bin-run.RITuNJ` |

同一张 SD `938a6da0…`。因此 **原 `0xfffffec0` 触发点是诊断 TCB probe 固件，不是当前生产 Home/Scene 源码。** 不要把 probe 二进制的崩溃当成产品回归；不要把 TCB 偏移探测留在 `onEnter`。当前源码无需为该类崩溃再改 `HomeActivity.cpp`。

## 2026-08-31 — 番茄打开后 Home 有书名无封面：171×254 与 Scene 110×180/74×106 对不上

在 patched QEMU 里安装当前 `com.fanqie.client` 1.3.3，拉都市异能书单 24 本，打开
`开局地摊卖大力`（`m4cp://fanqie/6838480082219043843`），读第 1 章后再回 Home。

### 现象
- 只进详情、不进阅读器：Home 仍是空最近阅读。`updateProviderBook()` 只更新已有行，
  不会 `addBook`；真正写入 `recent.bin` 的是 `TxtReaderActivity` 开读。
- 开读后 Home 有书名/作者/`继续阅读`/`0%`，hero 与 mini 封面位是空框。
- SD 上封面缓存是 `cover_171x254.bmp`（Fengyan `homeCoverWidth/ThumbHeight`），
  `recent.bin` 的模板是 `cover_[WIDTH]x[HEIGHT].bmp`。Scene Home 解码要
  `110×180` 与 `74×106`（`HomeScene::kHomeCurrentCover*` / `kHomeRecentCover*`）。
  `tryEnsureCoverThumbInCtx` 现在会在绑定 Home 时从同目录 `source.img` 按需生成
  `cover_110x180.bmp` / `cover_74x106.bmp`（JPEG 走 1-bit exact-size，不下载、
  不预写全部书的备用图）。不要用 2-bit 的 171×254 当缩放源。
- 详情页本身是文字优先，没有大封面；QEMU 出站 `-nic user,model=open_eth` 即可拉目录/正文，
  不要用 m4sim 默认 `hostfwd :18080`（本机 mihomo 占用会让 QEMU 起不来）。

运行目录 `/tmp/m4sim-home-covers.xYllm9`。截图 `home-after-read.png`。

## 2026-08-31 — Home Scene 封面按需从 source.img 生成，不要预写两档 BMP

- 下载原图是 JPEG `source.img`（这次实测 225×300 / ~23 KiB），详情路径再转 Fengyan
  `cover_171x254.bmp`（2-bit，header 宽度可能被 pad 成 190，Scene 解码要求精确
  110×180 / 74×106 且不接受 2-bit）。
- Home 绑定走 `M4ProviderCoverCache::ensureSizedCoverFromSource`：只看同目录
  `source.img`，缺哪档生成哪档，不 HTTP。JPEG 必须用
  `jpegFileTo1BitBmpStreamWithSize` 的 exact-size aspect-fill，否则输出 135×180
  一类尺寸，decoder 会因 `w != expW` 丢掉封面。
- 现有 `.pio` qemu_plugin 二进制不含此改动；要看 Home 真封面必须重新编
  `murphy_m4_qemu_plugin`。
- 2026-08-31 视觉复核：`murphy_m4_qemu_plugin` firmware.bin
  `efed32f86bda4a42632e2e8c48a463a49cbff08ff8e800b304498daa998c3c4b`
  5,562,768 B；私有 flash mirror `0d28ebb8…`；SD 拷自
  `/tmp/m4sim-home-covers.xYllm9`（已有 recents + `source.img`，无 Scene BMP）。
  启动后 Home 绑定即写出 `cover_110x180.bmp` 2942 B（1-bit 110×-180）和
  `cover_74x106.bmp` 1334 B（1-bit 74×-106）。QEMU Home 大封面/小封面都有
  `开局地摊卖大力` 图，不再是空框。运行目录
  `/tmp/m4sim-home-covers-fix.82YCwn`，截图 `home-boot.png`。
  不必再开番茄目录；有 `recent.bin` + `source.img` 即可证明 generate-on-miss。

## 2026-08-31 — Wi-Fi「Connected, but could not save」是 mkdir 把已存在目录当失败

- 现象链：设置里连上路由器 → 红字 `Connected, but could not save Wi-Fi` → 微信读书
  `no_saved_wifi` / 确认键重试。根因不是射频，是凭据没写上 SD。
- `WifiCredentialStore::saveCredentials` 曾 `if (!SdMan.mkdir("/.crosspoint")) return false;`。
  SdFat 在目录**已经存在**时 `mkdir` 返回 false。主页 recents/设置第一次启动就会创建
  `/.crosspoint`，所以之后每一次 Wi-Fi 保存都失败。其它 store（RecentBooks、Settings、
  KOReader）都忽略 mkdir 返回值。
- 修复：仅在 `!exists("/.crosspoint")` 时 mkdir；保存失败不再把已连通的 STA 标成
  `CONNECTION_FAILED`（本会话 `net.isConnected()` 仍可用）。
- 回归：`/tmp/test_wifi_state`（g++-14 + `wifi_store_shims`）和
  `python3 simulator/tests/test_wifi_state_contract.py`。不要再把「mkdir 失败」当成
  凭据事务失败，除非目录确实不存在且创建失败。
- 这种一行契约 bug 不要开子代理。

## 2026-08-31 — 小改动：先 PIO，再停 QEMU

抽屉「四个字显示全」是十来行修补，产品正确，过程偏慢（约 8–10 分钟）。慢的不是 QEMU：`--ready-seconds 20 --detach` 第 2 次 ping（约 8s）就 READY，点「更多」+ `ROTATE_270` 截图约 3s。慢的是改之前读太广、改之后先停模拟器再全量编。

### 不要做
- 为十来行开 Muse / Luna。调度往返比改代码久。
- 在协调者树 `m4-critical-ui-home` 里搜 `AppListActivity` 宫格。Luna 合入后的抽屉在 **integration**（`m4-home-orch-integration`）。协调者树没有那份文件，grep 空结果会空转一轮。
- 先读 `M4UiTextPolicy` / 抽新政策头 / 加源码扫描 pytest，再改那一行 `truncated(..., tile.width-8)`。调用点改掉、两条断言（4 字原样、5 字加 `…`）就够。
- 先 `./m4sim stop` 再 PIO。串行停机把 4 分钟编译变成「停机 + 编译 + 开机」。
- host `g++` 的 cwd 留在协调者树。integration 的新 `test_utf8_ellipsize.cpp` 会报 No such file。
- `--ready-seconds 120`、对 `--keep-alive` 做 120s `get_command_or_subagent_output`。

### 一次做对
1. 在 **integration** 改调用点（或当前真正跑 QEMU 的那棵树）。
2. 立刻 `PLATFORMIO_HOME_DIR=/tmp/pio_home2 pio run -e murphy_m4_qemu_plugin -j1`。QEMU 若在跑，让它接着跑。
3. 同一时刻在 integration cwd 跑 host 测：`g++-14 -std=c++17 -I firmware/lib/Utf8 firmware/lib/Utf8/Utf8.cpp firmware/tests/native_app/test_utf8_ellipsize.cpp`。
4. PIO SUCCESS 后：`M4SIM_TMP=/tmp/m4sim-home-r2-dock ./m4sim stop`，然后 `./m4sim run --plugin-debug --skip-build --no-hostfwd --ready-seconds 20`（默认 `--detach`）。
5. `m4adb tap 437 611`（「更多」），`/usr/bin/python3` + PIL `ROTATE_270`，`share-image`。

目标墙钟：改完到截图 ≈ PIO 时长 + 15s，而不是 PIO + 停机等待 + 探索轮次。

## 2026-08-31 — Home mini 不要重复 hero；书名最多两排

产品：四本最近阅读应是 1 hero + 3 本不同的书；封面书名最多两排，超出才 `…`。

### 绑定
- `setCurrent(books[0])` 之后，`addRecent` 和 mini cover decode 都必须从 `books[1]` 起，`itemIndex` 0..2。`homeRecentBooksCount=4` 才能填满 3 个 mini。
- 跳过 hero 之后，`kActionOpenCurrentBook` 不能再读 `snapshot.recent[0]`（那是第一本 mini）。改用 `snapshot.currentPath` / `currentOriginalSource`。
- cover key 必须跟 `addRecent` 下标对齐，否则会张冠李戴。

### 排版
- 编译器把 ellipsis 打在 text payload offset+11；runtime 以前跳过这个字节。要写进 `RenderEvent.ellipsis`。
- `GfxSceneRenderer` 以前一次 `drawText` 不裁切，mini 书名会串列。`drawSceneText`：rect.h>20 且 (h≥2×lineH 或 h≥36) 才允许 2 行；装得下就原样画，装不下才在最后一行加 `…`。不要 `#include M4UiText.h`（绑死 `GfxRenderer`，SpyGfx 编不过）。
- hero 标题槽 `[209,142,220,52]`，author/source 下移到 198/220；mini `$item.title` `[0,136,129,44]`，`item_height` 184。改完必须 `compile_home_theme.py --emit-header`。
- SpyGfx `getTextWidth` 恒 50、`truncatedText` 恒 `"..."`。短字面 "A"/"B" 必须走单行原串；wrap 测用每码点 10px 的 WidthSpy。
- 几何锁：`test_murphy_default_exact_geometry.py` / `test_home_typography_polish.py` / `test_home_font_hierarchy.py` / `test_murphy_default_target_geometry.py`。`test_home_scene_runtime.cpp` 里 `$recent` x=42 y=405 是旧主题坐标，与当前 28/380 不一致，不要当回归绿。

## 2026-08-31 — Settings Hub ui_24_bold 编译过但 QEMU 空白：FONT_MAP ≠ runtimeFontId

- `compile_home_theme.py` FONT_MAP 里 `ui_24_bold` → 25 可编译，`murphy_settings_hub_m4theme.h` 也生成，但 `GfxSceneRenderer::runtimeFontId` 只把 12→UI_12、13→UI_12、14/15→NOTOSANS_14、16/17→NOTOSANS_16、18/19/20/21/22/23→NOTOSANS_18，`default` 原样返回 25，`GfxRenderer::getTextWidth(25)` 找不到字体直接不画，QEMU 卡片标题全白。
- 已验证：`grep -R UI_.*_FONT_ID firmware/src/fontIds.h` 只有 `UI_10` (11) 和 `UI_12` (13) 声明，`UI_14` 等走 NOTOSANS。Hub 页标题 `ui_20_bold` (21→18) 可画，分类 `ui_24_bold` (25) 不可。改为 `ui_22_bold` (23→18) 后 100px 卡片文字在 QEMU 正常出现，且与 `kSettingsHubItemH` 100 居中 `[24,32,380,36]` 对齐。
- 一次性正确做法：改前先 `grep UI_.*_FONT_ID` + `runtimeFontId` switch，选已声明且映射到最大 NOTOSANS_18 的 `ui_22`（不要发明 `UI_14`），改后 QEMU 截 Hub 再验 `01-home`/`03-hub` 是否有文字与 `返回|主页|历史`。

## 2026-08-31 — Scene repeat actions must reach child hit regions

- `UiSceneRuntime` previously kept an action only on a repeat prefix. The repeated cover/title render events therefore had no action metadata, so Home cards and Settings rows could look present while taps were ignored.
- Pass effective action metadata through groups and repeats. For actionable repeated covers/icons, preserve the geometry while an asset binding is unavailable so the renderer can show a placeholder and the hit target remains live.
- Regression requires both host hit-test coverage and a QEMU `m4adb tap` followed by `m4adb ui` and a screenshot; a production compile alone does not prove the interaction.

## 2026-09-01 — Non-Home header title vs status ink + USB flash stalls

- Device AppList with `HeaderSafeTop=20` / `HeaderTitleBaseline=31` / `HeaderH=46`: title first ink **y=25**, status/wifi first ink **y=35** (~10px title-high). Raising SafeTop alone moves the whole header via `dy=rect.y`; wifi/battery sit at `rect.y + (HeaderH-icon)/2`. Human asked only to move **title text**; SafeTop 12→20 and interim 28/38/52 dropped the entire status bar. Device Home status first-ink ≈24 vs AppList ≈35.
- Correct control: **`HeaderSafeTop = 0`** (bar back at original/Home-adjacent Y) and **`HeaderTitleBaseline`** only (38 inside the 46px band). Do not raise SafeTop or HeaderH to “create air”. Fengyan `.headerHeight` stays `HomeRef::HeaderH`.
- Round 9 labeled sheet (`docs/orchestration/assets/icon-design-sheet-round9.png`) maps **番茄小说=tomato (r3c1)** and **开源阅读=open book (r3c3)**. Luna mapped the open book onto `com.fanqie.client` because the prompt said “three distinct book-like cells”. Do not override labeled cells. Glyph crop must exclude caption (tomato ink bbox `(132,731,301,895)`); caption starts ~y=932.
- `x=40..200` first-ink on AppList includes the vertical divider at `rect.y+12` — that is not title glyph top.
- `flash_app1_once.sh` / any `awk '/m4adb\.py/'` kill will SIGTERM **any** shell whose argv still contains that substring (heredoc flash scripts, interactive wrappers). Put flash/daemon orchestration in a **file** and match only `Python .../m4adb.py daemon` PIDs.
- `/dev/cu.usbmodem101` present ≠ flashable: esptool "No serial data received" + bridge "等待响应超时" means CDC enumerated but app/JTAG silent. pyserial open/DTR toggle can still read 0 bytes. Do not loop APP1 writes; ask for USB re-seat / hardware reset, then one flash.
