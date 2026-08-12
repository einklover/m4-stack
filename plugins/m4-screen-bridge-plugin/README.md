# Murphy M4 屏幕桥插件

Android 无障碍屏幕桥的 M4 原生 XML 阅读插件。手机端在 TCP 48624 提供应用目录、
结构化小红书内容和兼容的截图流；本插件复用固件 XML 排版、列表和 `flowText` 分页。

## 职责

- 原生 provider id 为 `screenbridge`，声明 `network` 与图片缓存所需的 `filesystem.appdata` 权限。
- 应用页显示手机程序目录；小红书进入结构化阅读模式。
- 推荐页是已预取完成的图文阅读清单。
- 正文和评论用系统 `flowText` 本地分页；图片使用 1bpp BMP 单页显示并支持左右切换。
- 视频、直播不进入阅读清单。
- 与设备通信的完整格式见 `docs/SCREEN_BRIDGE_PROTOCOL.md`。

## 打包

```bash
python3 tools/package.py <src-dir> <out.m4x>
```

或使用 `resolve_package(src_dir, cache_dir)` 生成内容哈希命名的 `.m4x`。
插件包只包含 `manifest.json` 与清单声明的文件，不携带二进制产物。

## 发布状态

协议未做认证，仅适用于可信局域网（见协议文档安全说明）。
