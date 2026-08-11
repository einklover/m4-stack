-- com.jjwxc.client — 晋江文学 M4x plugin.
-- 数据: androidapi (UTF-8 JSON) + WAP (VIP 章, 扫码登录后)。
-- Loads: storage/auth/catalog/layout/api/gbk/content_provider/ui_* via sys.load

sys.load("storage.lua")
sys.load("auth.lua")
sys.load("catalog.lua")
sys.load("layout.lua")
sys.load("api.lua")
sys.load("gbk.lua")
sys.load("content_provider.lua")
sys.load("ui_template.lua")
sys.load("ui_login.lua")
sys.load("ui_shelf.lua")
sys.load("ui_category.lua")
sys.load("ui_booklist.lua")
sys.load("ui_toc.lua")
sys.load("ui_reader.lua")

sys.load("app_runtime_a.lua")
sys.load("app_runtime_b.lua")
sys.load("app_runtime_c.lua")
sys.load("app_runtime_d.lua")
