# Murphy M4 原厂固件修改经验与避坑指南

> 适用范围：Murphy M4 / ESP32-S3 原厂固件二进制补丁、双 OTA 分区切换、设置菜单扩展、原厂确认框复用。
>
> 核心原则：**每次都从原始原厂 BIN 开始，不叠加旧补丁；优先复用原厂已有业务状态机；任何绝对地址都只作为当前版本参考，下一版必须重新定位。**

---

## 1. 目标应如何抽象

本项目真正需要实现的不是“回滚固件”，也不是固定启动某个分区，而是：

```text
当前运行 ota_0 -> 切换到 ota_1
当前运行 ota_1 -> 切换到 ota_0
```

理想交互：

```text
设置 -> 常用
    ...
    传书
    切换分区
```

点击“切换分区”后：

```text
切换分区
  -> 原厂确认框
  -> 用户确认
  -> 动态确定另一个 OTA 分区
  -> esp_ota_set_boot_partition(target)
  -> 更新 otadata
  -> 重启
```

要求：

- 不覆盖“传书”等现有功能；
- 不固定写死 app0 或 app1；
- 不擦写两个 APP 分区；
- 不修改无关设置的确认流程；
- 尽量复用原厂已有 OTA 校验、确认、错误处理和重启逻辑。

---

## 2. 第一原则：永远从原始原厂 BIN 重做

错误做法：

```text
factory.bin -> v1 -> v2 -> v3 -> v4
```

正确做法：

```text
factory.bin
   -> patch_vNext.py
   -> factory_dualboot_vNext.bin
```

原因：

- 二进制补丁叠加会继承旧版本中的错误；
- 很难判断当前异常来自哪一代补丁；
- 地址、字符串、菜单表一旦移动，旧 patch 很容易静默写错位置。

每一版建议至少保留：

```text
factory_xxx.bin
patch_dualboot_xxx.py
factory_xxx_dualboot.bin
patch_report.txt
```

并记录原始和修改后的 SHA-256。

---

## 3. 设置菜单通常不是“一张数组”

这次最典型的坑是：一个设置页可能同时存在多套数据结构。

常见组成：

```text
显示条目数组
点击 / action 数组
条目数量
最大选中索引
字符串资源
右侧 value renderer
图标 renderer
enable / disable 判断
action handler
确认框状态
```

因此，新增第 8 项不能只把：

```text
[25, 216, 141, 31, 123, 253, 218]
```

改成：

```text
[25, 216, 141, 31, 123, 253, 218, 153]
```

还要同步检查：

- UI 显示列表；
- 点击分发列表；
- count；
- max index；
- 该 ID 对应的特殊 renderer；
- handler；
- 是否存在多个同内容数组副本。

### 实机症状与对应问题

如果出现：

```text
显示的是“传书”
点击却执行“切换分区”
```

优先怀疑：

- display/action 两套数组不同步；
- action ID 被复用；
- 点击索引与显示索引错位。

---

## 4. 不要随便复用旧 action ID

旧 ID 即使当前没有出现在菜单中，也可能仍参与：

```text
右侧状态文本
Wi-Fi 名显示
图标
状态判断
隐藏菜单
确认框逻辑
```

例如这次复用了原本网络相关的 dormant ID 后，出现了：

```text
切换分区    MyWiFi
```

原因不是标题字符串有问题，而是 renderer 仍然认为该 ID 属于网络类设置。

### 正确做法

对候选 ID 做“全引用检查”：

```text
搜索该 ID
 -> 数据数组引用
 -> compare / branch 引用
 -> value renderer
 -> icon renderer
 -> enable/disable
 -> action handler
 -> confirmation callback
```

结论：**一个 action ID 的语义绝不只由点击 handler 决定。**

---

## 5. 最大的坑：共享代码汇合点

反汇编里看到：

```asm
...
call8 0x4201xxxx
...
```

不能立刻认为它只属于当前菜单项。

实际结构可能是：

```text
ID A --\
ID B ---\
ID C ----> shared code -> confirmation()
ID D ---/
ID E --/
```

如果直接把 shared code 改成：

```asm
call8 switch_partition
```

后果就是：

> 所有经过这个共享确认入口的设置都会变成“切换系统”。

### 实机故障特征

```text
任何设置只要弹出确认框
点击确认后都切换系统
```

几乎可以直接判断：**改到了通用确认框汇合点。**

### 必做检查

修改任何代码地址前，先确认：

- 这个地址有几个 predecessor？
- 谁会跳入这里？
- 这是独占分支还是共享 basic block？
- 修改后是否改变其他 action 的 CFG？

原则：

```text
XREF / predecessor 数量 > 1
=> 高风险共享点
```

---

## 6. Xtensa windowed ABI 是必须重视的坑

ESP32-S3 使用 Xtensa windowed ABI。

下面两种写法在语义上**不一定等价**：

```asm
A:
    call8 C
```

和：

```asm
A:
    call8 B
B:
    entry ...
    call8 C
```

因为 `CALL8 + ENTRY` 会发生 register window rotation。

即使你在 B 里“重新设置了 a10/a11/a12”，调用现场也可能已经与原代码不同。

### 本项目中的实际经验

第一版实机成功的调用现场大致是：

```asm
l32r   a10, UI_CONTEXT
movi.n a12, 0
movi.n a11, 14
call8  OriginalSwitchFunction
```

后来为了隔离逻辑加了一层 trampoline：

```text
SettingsHandler
  -> call8 trampoline
  -> entry
  -> call8 OriginalSwitchFunction
```

虽然确认框能出现，但实际切换行为发生变化。

### 结论

如果某个实机成功调用已经被证明有效，后续应尽量保持：

- 同一调用层级；
- 同一 register window；
- 同样的参数准备；
- 同样的返回路径。

不要仅凭“C 语言语义看起来等价”就增加函数层。

---

## 7. Code cave：优先用 J，不要轻易加 CALL8 层

如果原位置空间不足，需要跳到 code cave，优先考虑：

```asm
j code_cave
```

然后在 code cave 中：

```asm
l32r   a10, ...
movi.n a12, 0
movi.n a11, 14
call8  original_function
j      return_address
```

相比：

```asm
call8 trampoline
```

`j` 不会新建 register window，更接近原始调用现场。

---

## 8. OTA 语义必须是动态 0 <-> 1

目标逻辑应明确写成：

```cpp
const esp_partition_t* running = esp_ota_get_running_partition();

esp_partition_subtype_t targetSubtype =
    running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0
        ? ESP_PARTITION_SUBTYPE_APP_OTA_1
        : ESP_PARTITION_SUBTYPE_APP_OTA_0;

const esp_partition_t* target = esp_partition_find_first(
    ESP_PARTITION_TYPE_APP,
    targetSubtype,
    nullptr
);

esp_ota_set_boot_partition(target);
esp_restart();
```

禁止写成：

```cpp
set_boot_partition(ota_0);
```

或：

```cpp
set_boot_partition(ota_1);
```

### 当前 M4 分区语义参考

```text
app0 / ota_0
app1 / ota_1
```

实际绝对偏移和大小应以该版本 partition table 为准。

---

## 9. 优先复用“升级成功后的后半段状态机”

如果原厂已经存在完整 OTA 更新逻辑，通常最稳的方案不是重写：

```text
校验
写 otadata
错误提示
重启
```

而是定位：

```text
写入固件完成
  -> 标记成功
  -> 选择下一 OTA
  -> set_boot_partition
  -> reboot
```

如果业务允许，可考虑：

> 跳过真正写固件步骤，直接进入“写入成功后的切槽收尾逻辑”。

但必须确认：

- 收尾逻辑确实动态选择另一 OTA；
- 没有依赖写入阶段留下的重要上下文；
- 不会因为镜像校验状态缺失而回退；
- 不会修改到通用 OTA 升级流程。

---

## 10. 如何识别 ESP-IDF OTA 函数

原厂二进制往往没有符号，但 ESP-IDF 自带大量稳定字符串和 assert。

可利用：

```text
ota_app_count < 16
start_from != NULL
must erase the partition before writing to it
```

通过字符串 XREF 回溯，很容易定位到：

```text
esp_ota_get_next_update_partition()
esp_ota_get_running_partition()
esp_ota_get_partition_description()
esp_ota_set_boot_partition()
```

识别依据：

- assert 字符串；
- 错误码；
- 函数调用关系；
- 参数检查模式；
- ESP-IDF 源码实现。

ESP-IDF 源码在这类逆向中可以看作“符号数据库”。

---

## 11. 当前版本已识别地址只能作为 fingerprint

某一版固件中识别出的：

```text
0x42014CF8
0x420D2160
0x420D21B8
0x420D20BC
```

只能作为：

- 当前版本调试参考；
- signature 对照；
- 控制流 fingerprint。

**下一版严禁直接 hardcode 使用。**

新版必须通过：

```text
字符串
XREF
函数结构
ESP-IDF assert
CFG
```

重新定位。

---

## 12. 新版固件迁移标准流程

### Step 1：记录原始文件信息

```bash
shasum -a 256 firmware.bin
```

保存：

- 文件大小；
- SHA-256；
- 版本号；
- 来源。

---

### Step 2：解析 ESP app image

确认：

```text
magic
segment count
entry address
segment table
checksum
hash_appended
```

不要一上来就做 hex patch。

---

### Step 3：搜索稳定字符串

优先搜索：

```text
切换上一个固件
Switch to previous firmware
切換上一個韌體
传书
配置网络
Connect Network
```

中文建议直接 Python UTF-8 搜索。

---

### Step 4：从字符串反查资源表和 handler

推荐链路：

```text
字符串
  -> localization/resource table
  -> resource/action ID
  -> settings dispatcher
  -> confirmation state
  -> OTA function
```

---

### Step 5：定位 Common 设置数组

如果旧版本菜单 ID 已知，可把整段作为 fingerprint。

例如：

```python
ids = [25, 216, 141, 31, 123, 253, 218]
pattern = b''.join(struct.pack('<I', x) for x in ids)
```

然后搜索所有命中。

**不要只取第一个。**

---

### Step 6：列出菜单数组的所有副本

示例：

```python
pos = 0
while True:
    off = data.find(pattern, pos)
    if off < 0:
        break
    print(hex(off))
    pos = off + 1
```

每一个命中都要判定用途：

```text
显示
点击
缓存
默认值
调试数据
```

---

### Step 7：确认 count / max index

有些 UI 保存的是条目数量：

```text
7
```

有些保存的是最大索引：

```text
6
```

新增一项时要先确认语义，再改：

```text
count: 7 -> 8
max index: 6 -> 7
```

禁止看到类似 `[6,7,9]` 就凭经验直接改字节。

---

### Step 8：检查新增 ID 的所有 XREF

必须搜索：

```text
比较指令
跳转表
renderer
value getter
action handler
confirmation callback
```

如果该 ID 原本属于别的功能，必须处理旧 renderer 副作用。

---

### Step 9：分析 CFG 后再 patch

检查：

```text
predecessors
successors
shared basic blocks
call graph
```

特别警惕：

```text
多个 action 最终跳到同一个 call8
```

---

### Step 10：优先保持原厂确认框

理想方式：

```text
新增项
 -> 进入独占业务入口
 -> 设置原厂已有的业务状态
 -> 原厂确认框
 -> 原厂 confirm callback
 -> 原厂 set_boot_partition
 -> 原厂 reboot
```

不要为了“方便”改通用确认框核心函数。

---

### Step 11：重新计算 ESP image 校验

如果修改了任何字节，需要重新处理：

```text
image checksum
padding
appended SHA-256
```

如果：

```text
hash_appended = 1
```

必须重算 appended SHA-256。

否则可能出现：

```text
bootloader 拒绝启动
OTA 镜像校验失败
```

---

## 13. Code cave 使用原则

优先级：

```text
真正 padding
  > 未引用区域
  > 长 debug/assert string 尾部
  > 覆盖活跃代码
```

如果利用字符串尾部：

原始：

```text
ota_app_count < 16 && "must erase the partition before writing to it"
```

可以考虑缩短为：

```text
ota_app_count < 16
```

再利用尾部空间。

但必须先确认：

- 没有指针指向字符串中间；
- 没有长度常量依赖完整字符串；
- 不影响格式化输出。

---

## 14. 使用 code cave 前必须做 XREF 检查

至少搜索：

```python
struct.pack('<I', cave_address)
```

以及 cave 区间内的可能地址。

如果只有：

```text
pointer -> string start
```

通常更安全。

如果出现：

```text
pointer -> string + N
```

不要复用该尾部区域。

---

## 15. 补丁脚本必须带断言

禁止：

```python
buf[offset:offset+3] = patch
```

推荐：

```python
expected = bytes.fromhex('AA BB CC')
assert buf[offset:offset+len(expected)] == expected
buf[offset:offset+len(expected)] = patch
```

这样新版固件结构发生变化时，会直接失败，而不是静默写坏。

建议所有 patch 都使用：

```text
expected original bytes
new bytes
purpose
```

三元记录。

---

## 16. 自动生成 Patch Report

每次构建建议输出：

```text
Base firmware SHA256
Patched firmware SHA256
File size

Patch #1
VA:
File offset:
Before:
After:
Reason:

Patch #2
...

ESP checksum:
Appended SHA256:
Menu arrays:
OTA function candidates:
Code cave range:
```

这样几个月后迁移新版时，不需要重新从 Python 代码猜当时做了什么。

---

## 17. 推荐工具链

### Python

核心模块：

```python
struct
hashlib
pathlib
re
```

用于：

```text
二进制搜索
地址换算
patch
checksum
SHA-256
生成报告
```

### Xtensa 小型 decoder

建议长期保留一个轻量工具，例如：

```text
tools/xtmini.py
```

至少支持：

```text
call8
j
l32r
movi.n
entry
retw.n
```

非常适合快速看局部控制流。

### Ghidra

推荐用于：

```text
XREF
CFG
function boundaries
call graph
shared basic block
```

遇到“所有确认框都被改坏”这类问题时，CFG 比单纯反汇编文本直观得多。

### ESP-IDF 源码

用于函数识别和语义确认。

### strings / grep

```bash
strings firmware.bin | grep -i ota
strings firmware.bin | grep firmware
```

### xxd

```bash
xxd -g 1 -s OFFSET -l LENGTH firmware.bin
```

---

## 18. 建议建立永久工具目录

建议仓库中增加：

```text
tools/stock_firmware_patch/
├── README.md
├── xtmini.py
├── esp_image.py
├── find_strings.py
├── find_xrefs.py
├── find_menu_arrays.py
├── patch_dualboot_template.py
└── signatures.json
```

### signatures.json 不要记录死地址

推荐记录：

```json
{
  "ota_strings": [
    "Switch to previous firmware (%d)",
    "ota_app_count < 16"
  ],
  "common_menu_ids": [
    25,
    216,
    141,
    31,
    123,
    253,
    218
  ]
}
```

新版根据 signature 自动扫描。

---

## 19. 本次各失败版本留下的经验

### 第一版

优点：

```text
原厂 OTA 切换实机成功
确认框正常
```

问题：

```text
修改了共享 branch
导致其他确认框全部进入切换系统
```

经验：

> 成功业务入口可以复用，但必须把它从共享控制流中隔离出来。

### 菜单扩展错误版

问题：

```text
传书和切换分区互相污染
```

经验：

> 设置菜单存在不止一份数组/结构。

### 绕开确认框版本

优点：

```text
其他确认框恢复正常
```

问题：

```text
只重启，不真正切换
```

经验：

> 不要在不完全理解 OTA 状态机时自行拼接收尾逻辑。

### CALL8 trampoline 版本

优点：

```text
确认框重新出现
```

问题：

```text
确认后仍未正确切换
```

经验：

> Xtensa register-window ABI 下，多一层 CALL8 不是透明操作。

---

## 20. 实机验证顺序

每个新版本不要直接先测切换。

建议按以下顺序：

### A. 基础启动

- 固件可以启动；
- 设置页可以进入；
- 无 bootloop；
- 无 panic。

### B. 原有设置回归

重点测试：

- 传书；
- Wi-Fi；
- 任何会弹确认框的设置；
- 其他常用设置。

确认：

```text
原有设置行为与原厂一致
```

### C. 新菜单 UI

确认：

```text
传书仍然存在
切换分区是独立第 8 项
右侧无错误 Wi-Fi 名/状态值
```

### D. 切换功能

从 ota_0 测：

```text
ota_0 -> ota_1
```

再从 ota_1 测：

```text
ota_1 -> ota_0
```

两边都成功，才能证明不是固定切某个槽。

### E. 二次回归

切回原系统后，再测试：

- 传书；
- Wi-Fi；
- 确认框；
- 其他设置。

---

## 21. 发布前检查清单

- [ ] 补丁基于原始原厂 BIN，而不是旧 patch 版本
- [ ] 原始 SHA-256 已记录
- [ ] 修改后的 SHA-256 已记录
- [ ] 所有 patch 都验证 expected original bytes
- [ ] Common 显示数组已扩展
- [ ] Common action 数组已扩展
- [ ] count / max index 已同步
- [ ] “传书” ID 和 handler 未被修改
- [ ] 新 ID 的所有 XREF 已检查
- [ ] 新 ID 的 value renderer 已检查
- [ ] 未修改共享确认框入口
- [ ] 未破坏其他 action 的 CFG
- [ ] OTA 目标不是固定 ota_0 或 ota_1
- [ ] 切换逻辑为当前槽 <-> 另一槽
- [ ] ESP image checksum 已重算
- [ ] appended SHA-256 已重算
- [ ] 固件可以正常启动
- [ ] 其他确认框实机正常
- [ ] 传书实机正常
- [ ] ota_0 -> ota_1 实机验证
- [ ] ota_1 -> ota_0 实机验证
- [ ] 保留恢复/重新刷机手段

---

## 22. 下一版迁移时可直接使用的任务描述

拿到新版原厂固件后，可以直接使用下面这段要求：

> 基于最新原厂 firmware.bin 重新迁移 Murphy M4 双系统补丁。不要沿用旧版绝对地址，必须通过字符串、XREF、CFG 和 ESP-IDF signature 重新定位。保留所有原厂设置，在“常用”最后真正新增“切换分区”，不得覆盖“传书”。检查 display/action/count/max-index/value-renderer/handler 全部引用。切换必须动态实现当前 ota_0 -> ota_1、当前 ota_1 -> ota_0，不固定某一槽。优先复用原厂确认框和 OTA 成功收尾状态机，不得修改共享确认框调用点。注意 Xtensa windowed ABI，避免额外 CALL8 trampoline 改变调用现场。修改完成后重新计算 ESP image checksum 和 appended SHA-256，并输出 patch report，再进行双向实机验证。

---

## 23. 最终方法论

不要把任务理解成：

> 在固定地址改几个字节。

正确理解应该是：

```text
用字符串 / signature 识别新版结构
  -> 找到菜单所有 representation
  -> 找到新增 ID 的所有 XREF
  -> 找到原厂 OTA 状态机
  -> 分析 CFG，避开 shared branch
  -> 保持 Xtensa 原调用 ABI
  -> 只 patch 独占路径
  -> 重新计算 ESP checksum + SHA256
  -> 静态验证
  -> 实机双向验证
```

只要这套流程保持不变，即使下一版原厂固件所有绝对地址都变化，也可以快速迁移，而不是重新从零逆向。
