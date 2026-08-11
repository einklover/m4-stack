# Murphy M4 屏幕桥插件

Android 无障碍屏幕桥的 M4 侧占位插件。手机端在 TCP 48624 提供截图，M4 通过
`/v1/page` 协议读取，阅读界面由系统屏幕桥活动接管，本插件不实现读者 UI。

## 职责

- 原生 provider id 为 `screenbridge`，声明 `network` 与端点持久化所需的 `filesystem.appdata` 权限。
- `main.xml` 为占位界面；阅读器 UI 由系统活动持有。
- 与设备通信的完整格式见 `docs/SCREEN_BRIDGE_PROTOCOL.md`。

## 打包

```bash
python3 tools/package.py <src-dir> <out.m4x>
```

或使用 `resolve_package(src_dir, cache_dir)` 生成内容哈希命名的 `.m4x`。
插件包只包含 `manifest.json` 与清单声明的文件，不携带二进制产物。

## 发布状态

协议未做认证，仅适用于可信局域网（见协议文档安全说明）。
