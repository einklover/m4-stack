# Murphy M4 晋江文学 plugin

晋江文学城内容提供器插件（`com.jjwxc.client`），基于晋江官方 App 接口（androidapi，UTF-8 JSON）+ WAP 网页（VIP 章正文），支持晋江小说阅读 App 扫码登录、分类浏览、章节目录、章节缓存与原生阅读器接管。

## 功能

- **发现**：分类浏览（古言/现言/幻言/古穿/奇幻/科幻/纯爱/百合/无CP/衍生/二次元等 70+ 频道，连载/完结双行）+ 今日限免。无需键盘输入。
- **目录**：`chapterList` 接口写入 SD（FileRows，千章书不占 Lua 堆），原生章节目录接管。
- **阅读**：
  - 免费章：androidapi `chapterContent` 明文 JSON（UTF-8），无需登录。
  - VIP 章：需扫码登录；服务端返回 code 1004 自动切换 WAP 路径（`m.jjwxc.net/book2/{novelId}/{chapterId}` + 会话 cookie），正文为 GB18030，经内置转换表（47KB，23940 双字节字符）转 UTF-8 后缓存。
- **登录**：晋江官方「扫码登录」（`my.jjwxc.net/backend/login/jjreader`）：取 jjreaderKey → 绘制二维码 → 轮询确认 → 吸收会话 cookie。用「晋江小说阅读」App（Android≥5.0.9.2 / iOS≥4.2.2）扫一扫即可，无需输入法。
- **书架**：本地收藏 + 阅读进度（继续阅读/历史续读），退出登录仅清除会话。

## 架构

```
main.lua              — 屏幕状态机（startup/shelf/category/booklist/toc/reader/login/...）
api.lua               — androidapi + WAP 抓取；dl.jsonGet/jsonToFile 宿主投影优先
auth.lua              — 扫码登录状态机 + cookie 持久化（config.json）
gbk.lua + gbk_table.bin — GB18030→UTF-8 转换（仅 VIP 章加载，用完释放）
content_provider.lua  — 章节流水线：缓存探测 → 抓取 → 原子写 → reader.openText
storage.lua/catalog.lua/layout.lua/ui_*.lua — 与 fanqie/weread 插件同源的通用层
```

## 接口来源

- 官方 App 接口：`app-cdn.jjwxc.net/androidapi/*`（novelbasicinfo / chapterList / chapterContent / bookstore/getFullPage）
- 扫码登录：`my.jjwxc.net/backend/login/jjreader/*`（login.php / callback.php）
- 参考实现：公开 GitHub 爬虫（lyc8503/jjwxcCrawler 的 App 接口与 UA、imashen/crawl_novel 的页面解析）与 Legado 晋江书源（频道 ID、VIP 章 WAP 路径）；宿主插件骨架复用本仓库 fanqie/weread 插件的已测通用层。

## 构建

```bash
cd publish/m4-jjwxc-plugin
python3 tools/package.py           # 仅校验工具
python3 -c "import sys; sys.path.insert(0,'tools'); from package import build_m4x; import pathlib; build_m4x(pathlib.Path('.'), pathlib.Path('com.jjwxc.client.m4x'))"
```

将 `com.jjwxc.client.m4x` 放入设备 SD 卡 `/apps_inbox/` 安装。

## 回归测试

```bash
cd fengyan-m4-device-rc1/simulator/build-gcc14
cmake --build . --target test_jjwxc_lua && ctest -R jjwxc_lua
```

覆盖：分类/限免解析、目录投影、免费章拼接、VIP 自动切换（cookie 携带、GB18030 解码、未购买/登录失效分支）、扫码登录全流程、书架→分类→书单→目录→原生阅读器→进度闭环。

## 已知限制

- 无键盘输入：只能通过分类/限免发现书籍；输入书号检索待宿主键盘接口开放。
- VIP 章需账号已购（晋江防盗要求 100% 购买比例）。
- WAP 页 GB18030 四字节扩展字符（生僻字）降级为 `?`，网络文学正文基本不涉及。
- 二维码 1 分钟有效，超时需重试。
