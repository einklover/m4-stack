from pathlib import Path

out = Path("build/m4ui-preview")
labels = {
    "home": "主页", "home_mem_warning": "内存不足",
    "my_library": "我的书架", "recent_books": "最近阅读",
    "reader_epub": "EPUB 阅读", "reader_xtc": "XTC 阅读", "reader_txt": "TXT 阅读",
    "reader_menu_epub": "EPUB 菜单", "reader_menu_xtc": "XTC 菜单",
    "chapter_epub": "EPUB 目录", "chapter_xtc": "XTC 目录", "chapter_txt": "TXT 目录",
    "reader_settings": "阅读设置", "percent_selection": "跳转选择", "font_selection": "字体选择",
    "epub_reader_settings": "EPUB 阅读设置",
    "bookmark_manager": "书签管理", "bookmark_notes": "书签笔记",
    "auto_page_turn": "自动翻页", "tilt_page_turn": "倾斜翻页",
    "settings_display": "设置-显示", "settings_controls": "设置-按钮", "settings_system": "设置-系统",
    "button_remap": "按键映射", "calibre_settings": "Calibre 设置", "calibre_connect": "Calibre 连接",
    "wifi_selection": "Wi-Fi 选择", "network_mode": "网络模式",
    "keyboard_entry": "键盘输入", "activity_with_sub": "子页面",
    "clear_cache": "清理缓存", "reset_settings": "还原设置",
    "data_capsule_settings": "数据胶囊配置", "data_capsule_browser": "数据胶囊浏览",
    "jianguo_yun_settings": "坚果云配置", "jianguo_browser": "坚果云浏览", "jianguo_sync": "晋江云同步",
    "koreader_auth": "KOReader 登录", "koreader_settings": "KOReader 同步", "koreader_sync": "KOReader 同步",
    "app_list": "应用列表", "app_install": "安装扩展", "app_runtime": "应用运行时", "native_app": "原生应用",
    "native_provider_book": "内容提供商", "native_provider_login": "登录", "native_provider_endpoint": "端点配置",
    "boot": "开机", "sleep": "休眠", "full_screen_message": "全屏消息",
    "ota_update": "系统升级", "online_ota": "在线更新", "developer_options": "开发者选项",
    "simple_bluetooth": "蓝牙设置", "screen_bridge": "屏幕桥接",
    "opds_browser": "OPDS 书库", "cross_point_web_server": "Web 服务器",
    "number_selection": "数值选择",
}

updated = {"reader_epub", "reader_xtc", "reader_txt", "epub_reader_settings"}

sections = [
    ("主页", ["home", "home_mem_warning"]),
    ("书架", ["my_library", "recent_books"]),
    ("阅读页", ["reader_epub", "reader_xtc", "reader_txt"]),
    ("阅读菜单", ["reader_menu_epub", "reader_menu_xtc"]),
    ("目录", ["chapter_epub", "chapter_xtc", "chapter_txt"]),
    ("阅读设置", ["reader_settings", "percent_selection", "font_selection", "epub_reader_settings"]),
    ("书签 & 翻页", ["bookmark_manager", "bookmark_notes", "auto_page_turn", "tilt_page_turn"]),
    ("系统设置", ["settings_display", "settings_controls", "settings_system"]),
    ("按键 & Calibre", ["button_remap", "calibre_settings", "calibre_connect"]),
    ("网络 & 实用", ["wifi_selection", "network_mode", "keyboard_entry", "activity_with_sub", "clear_cache", "reset_settings"]),
    ("数据 & 云同步", ["data_capsule_settings", "data_capsule_browser", "jianguo_yun_settings", "jianguo_browser", "jianguo_sync", "koreader_auth", "koreader_settings", "koreader_sync"]),
    ("应用", ["app_list", "app_install", "app_runtime", "native_app", "native_provider_book", "native_provider_login", "native_provider_endpoint"]),
    ("系统工具", ["boot", "sleep", "full_screen_message", "ota_update", "online_ota", "developer_options", "simple_bluetooth", "screen_bridge", "opds_browser", "cross_point_web_server", "number_selection"]),
]

html = '<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">'
html += '<title>Murphy M4 全 UI 预览</title><style>'
html += '*{box-sizing:border-box;margin:0;padding:0}'
html += 'body{font-family:system-ui,-apple-system,sans-serif;background:#f0f0f0;color:#222;padding:16px 24px}'
html += 'h1{font-size:24px;margin-bottom:2px}'
html += 'h1 span{font-weight:400;color:#888;font-size:15px}'
html += 'p.desc{color:#888;font-size:13px;margin:0 0 20px}'
html += '.sec{margin-bottom:28px}'
html += '.sec h2{font-size:16px;font-weight:600;padding:6px 0 8px;border-bottom:2px solid #ddd;margin-bottom:10px;color:#555}'
html += '.sec h2 em{font-weight:400;color:#999;font-size:13px}'
html += '.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(175px,1fr));gap:10px}'
html += '.card{background:#fff;border:1px solid #e0e0e0;border-radius:8px;padding:8px;box-shadow:0 1px 3px rgba(0,0,0,.06)}'
html += '.card:hover{box-shadow:0 3px 10px rgba(0,0,0,.12)}'
html += '.card .lbl{font-size:12px;font-weight:500;margin:0 0 2px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}'
html += '.card .fn{color:#aaa;font-size:10px;margin:0 0 5px}'
html += '.card img{width:100%;height:auto;border:1px solid #eee;border-radius:4px;image-rendering:pixelated;display:block}'
html += '.hl{background:#fff8e1;border-color:#f9d849;box-shadow:0 0 0 2px #f9d849}'
html += '.badge{display:inline-block;background:#f9d849;color:#000;font-size:9px;font-weight:700;padding:1px 5px;border-radius:3px;vertical-align:middle;margin-left:3px}'
html += '</style></head><body>'

html += '<h1>Murphy M4 全 UI 预览 <span>480×800 · 58 个屏幕</span></h1>'
html += '<p class="desc">⭐ 黄色高亮 = 本次更新的屏幕（已应用"正在阅读"卡片 + 阅读目标环）</p>'

for sec_name, names in sections:
    items = [(n, labels.get(n, n)) for n in names if (out / f"{n}.png").exists()]
    if not items:
        continue
    n_up = sum(1 for n, _ in items if n in updated)
    tag = f' <em>(✓ {n_up} 个更新)</em>' if n_up else ''
    html += f'<div class="sec"><h2>{sec_name}{tag}</h2><div class="grid">'
    for name, label in items:
        is_up = name in updated
        cls = 'hl' if is_up else ''
        badge = '<span class="badge">✓ 已更新</span>' if is_up else ''
        html += f'<div class="card {cls}"><div class="lbl">{label}{badge}</div><div class="fn">{name}.png</div><img src="{name}.png" alt="{label}"></div>'
    html += '</div></div>'

html += '</body></html>'
(out / "index.html").write_text(html, encoding="utf-8")
print(f"Gallery regenerated: {out / 'index.html'}")
