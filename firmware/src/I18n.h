#pragma once
/**
 * I18n - 多语言支持
 *
 * 使用 X-macro 模式，所有字符串集中定义在 STR_TABLE 中。
 * 自动生成 Str 枚举和两种语言的字符串数组。
 *
 * 用法：将硬编码中文替换为 L(Str::xxx)
 *   旧：drawText(... "系统设置" ...)
 *   新：drawText(... L(Str::kSystemSettings) ...)
 *
 * 新增字符串只需在 STR_TABLE 中添加一行：
 *   STR(key, "简体中文", "繁體中文")
 */

#include "CrossPointSettings.h"

// ═══════════════════════════════════════════════════════════════
//  字符串表定义（每种语言各一列，简体中文 | 繁體中文）
// ═══════════════════════════════════════════════════════════════
#define STR_TABLE \
  /* --- 通用按钮/操作 --- */ \
  STR(kBack,              "\xC2\xAB 返回",          "\xC2\xAB 返回") \
  STR(kBackShort,         "返回",            "返回") \
  STR(kConfirm,           "确认",            "確認") \
  STR(kCancel,            "取消",            "取消") \
  STR(kSelect,            "选择",            "選擇") \
  STR(kUp,                "上",              "上") \
  STR(kDown,              "下",              "下") \
  STR(kLeft,              "左",              "左") \
  STR(kRight,             "右",              "右") \
  STR(kDone,              "完成",            "完成") \
  STR(kRefresh,           "刷新",            "刷新") \
  STR(kConnect,           "连接",            "連接") \
  STR(kScan,              "扫描",            "掃描") \
  STR(kOpen,              "打开",            "打開") \
  STR(kDownload,          "下载",            "下載") \
  STR(kClear,             "清除",            "清除") \
  STR(kSet,               "设置",            "設置") \
  STR(kNextPage,          "下一页 »",        "下一頁 »") \
  STR(kLongPressRetry,    "长按重试",        "長按重試") \
  STR(kPleaseWait,        "请稍候...",       "請稍候...") \
  STR(kLoading,           "加载中...",       "載入中...") \
  STR(kRefreshing,        "刷新中...",       "刷新中...") \
  STR(kDownloading,       "下载中...",       "下載中...") \
  STR(kError,             "错误",            "錯誤") \
  STR(kFailed,            "失败:",           "失敗:") \
  /* --- 设置页通用 --- */ \
  STR(kSystemSettings,    "系统设置",        "系統設置") \
  STR(kApps,              "应用",            "應用") \
  STR(kInstallApp,        "安装扩展",        "安裝擴展") \
  STR(kCategoryDisplay,   "1)显示",          "1)顯示") \
  STR(kCategoryControls,  "2)按钮",          "2)按鈕") \
  STR(kCategorySystem,    "3)系统",          "3)系統") \
  STR(kOn,                "已开启",          "已開啟") \
  STR(kOff,               "已关闭",          "已關閉") \
  STR(kToggle,            "切换",            "切換") \
  /* --- 系统设置项名称 --- */ \
  STR(kSystemLanguage,          "系统语言",          "系統語言") \
  STR(kAutoSyncTimeOnBoot,      "开机自动同步时间",   "開機自動同步時間") \
  STR(kWifiAlwaysReselect,      "每次都重新选择WIFI", "每次都重新選擇WIFI") \
  STR(kDirectTxtRead,           "直读TXT文档",       "直讀TXT文檔") \
  STR(kSleepTimeout,            "休眠时间",          "休眠時間") \
  STR(kBluetoothSettings,       "蓝牙设置",          "藍牙設置") \
  STR(kJianGuoConfig,           "坚果云配置",        "堅果雲配置") \
  STR(kDataCapsuleConfig,       "数据胶囊配置",      "數據膠囊配置") \
  STR(kResetSettings,           "还原为初始设置",     "還原為初始設置") \
  STR(kClearCache,              "清理缓存",          "清理緩存") \
  STR(kSwitchBootSlot,          "切换启动区",        "切換啟動區") \
  STR(kApp0Official,            "APP0（官方）",       "APP0（官方）") \
  STR(kApp1Custom,              "APP1（自研）",       "APP1（自研）") \
  STR(kUnknownBootSlot,         "启动区未知",        "啟動區未知") \
  /* --- 系统 / 阅读动画 --- */ \
  STR(kSystemAnimation,         "系统动画",          "系統動畫") \
  STR(kPageTurnAnimation,       "翻页动画",          "翻頁動畫") \
  STR(kPageTurnAnimSteps,       "动画步数",          "動畫步數") \
  STR(kPageTurnAnimMult,        "窗口倍数",          "窗口倍數") \
  STR(kPageTurnAnimTp,          "动画波形TP",        "動畫波形TP") \
  STR(kPageTurnAnimFrameRate,   "动画帧率",          "動畫幀率") \
  STR(kPageTurnAnimDir,         "动画方向",          "動畫方向") \
  STR(kPageTurnFrSlow,          "慢(0x22)",          "慢(0x22)") \
  STR(kPageTurnFrMed,           "中(0x44)",          "中(0x44)") \
  STR(kPageTurnFrFast,          "快(0x88)",          "快(0x88)") \
  STR(kPageTurnDirR2L,          "右→左",             "右→左") \
  STR(kPageTurnDirL2R,          "左→右",             "左→右") \
  STR(kPageTurnDirB2T,          "下→上",             "下→上") \
  STR(kPageTurnDirT2B,          "上→下",             "上→下") \
  /* --- 系统语言选项 --- */ \
  STR(kLangSimplifiedChinese,   "简体中文",          "簡體中文") \
  STR(kLangTraditionalChinese,  "繁体中文",          "繁體中文") \
  /* --- 显示设置项名称 --- */ \
  STR(kSleepScreen,             "锁屏壁纸",          "鎖屏壁紙") \
  STR(kReadingProgressSetting,  "阅读进度",          "閱讀進度") \
  STR(kHideBatteryPercent,      "隐藏电池百分比",    "隱藏電池百分比") \
  STR(kRefreshFrequency,        "刷新频率",          "刷新頻率") \
  STR(kNeverFullRefresh,        "永不全刷",          "永不全刷") \
  STR(kButtonHints,             "按钮提示",          "按鈕提示") \
  STR(kFrontlightBrightness,    "前光亮度",          "前光亮度") \
  STR(kFrontlightWarmth,        "前光色温",          "前光色溫") \
  STR(kFullRefreshBeforeSleep,  "关机前全刷",        "關機前全刷") \
  STR(kImageQuality,            "图片质量",          "圖片質量") \
  STR(kIconStyle,               "图标风格",          "圖標風格") \
  STR(kIconSelectedStyle,       "图标选中风格",      "圖標選中風格") \
  /* --- 阅读设置项名称 --- */ \
  STR(kFontSize,                "字号",              "字號") \
  STR(kUiFontSize,              "系统字号",          "系統字號") \
  STR(kValAuto,                 "自动",              "自動") \
  STR(kFirstLineIndent,         "首行缩进",          "首行縮進") \
  STR(kLineSpacing,             "行间距",            "行間距") \
  STR(kWordSpacing,             "字间距",            "字間距") \
  STR(kTopMargin,               "上边距",            "上邊距") \
  STR(kBottomMargin,            "下边距",            "下邊距") \
  STR(kLeftMargin,              "左边距",            "左邊距") \
  STR(kRightMargin,             "右边距",            "右邊距") \
  STR(kReadingBackground,       "阅读背景",          "閱讀背景") \
  STR(kUnderline,               "下划线",            "下劃線") \
  STR(kUnderlineOffset,         "下划线偏移",        "下劃線偏移") \
  STR(kUnderlineStyle,          "下划线样式",        "下劃線樣式") \
  STR(kParagraphSpacing,        "段落间距",          "段落間距") \
  STR(kAlignment,               "对齐方式",          "對齊方式") \
  STR(kShowTime,                "显示时间",          "顯示時間") \
  STR(kShowEpubImages,          "显示epub图片",      "顯示epub圖片") \
  STR(kPunctWidth,              "标点宽度",          "標點寬度") \
  /* --- 按钮设置项名称 --- */ \
  STR(kSideButtonSettings,      "侧边按钮设置（仅阅读）", "側邊按鈕設置（僅閱讀）") \
  STR(kLongPressChapterSkip,    "长按左右键跳章节",   "長按左右鍵跳章節") \
  STR(kShortPowerButton,        "短按电源键",        "短按電源鍵") \
  STR(kLongPressBoot,           "长按开机",          "長按開機") \
  STR(kLongPressConfirmMenu,    "长按确认键打开菜单", "長按確認鍵打開菜單") \
  /* --- 枚举值翻译 --- */ \
  STR(kValDefaultBlack,         "默认黑",            "默認黑") \
  STR(kValDefaultWhite,         "默认白",            "默認白") \
  STR(kValTransparentWallpaper, "透明壁纸",          "透明壁紙") \
  STR(kValCustomBMP,            "自定义BMP",         "自定義BMP") \
  STR(kValCustomImage,          "自定义图片",        "自定義圖片") \
  STR(kValTransparentOverlay,   "透明叠加",          "透明疊加") \
  STR(kValNone,                 "无",                "無") \
  STR(kValNoProgress,           "不显示进度",        "不顯示進度") \
  STR(kValFullWithPercent,      "完整+百分比",       "完整+百分比") \
  STR(kValFullWithBookBar,      "完整+进度条",       "完整+進度條") \
  STR(kValOnlyBookBar,          "仅进度条",          "僅進度條") \
  STR(kValFullWithChapterBar,   "完整+章节条",       "完整+章節條") \
  STR(kValNever,                "从不",              "從不") \
  STR(kValInReader,             "仅阅读",            "僅閱讀") \
  STR(kValAlways,               "总是",              "總是") \
  STR(kValFast,                 "快速",              "快速") \
  STR(kValNormal,               "正常",              "正常") \
  STR(kValHD,                   "高清",              "高清") \
  STR(kValStyle1,               "风格一",            "風格一") \
  STR(kValStyle2,               "风格二",            "風格二") \
  STR(kValStyle3,               "风格三",            "風格三") \
  STR(kValCornersOnly,          "仅四角",            "僅四角") \
  STR(kValGrayBgOnly,           "仅灰色背景",        "僅灰色背景") \
  STR(kValDashedBorderOnly,     "仅虚线边框",        "僅虛線邊框") \
  STR(kValSolidBorderOnly,      "仅实线边框",        "僅實線邊框") \
  STR(kValCornersAndGrayBg,     "四角+灰色背景",     "四角+灰色背景") \
  STR(kValSmall,                "小",                "小") \
  STR(kValMedium,               "中",                "中") \
  STR(kValLarge,                "大",                "大") \
  STR(kValSolidLine,            "实线",              "實線") \
  STR(kValShortDash,            "短虚线",            "短虛線") \
  STR(kValMediumDash,           "中虚线",            "中虛線") \
  STR(kValLongDash,             "长虚线",            "長虛線") \
  STR(kValDotLine,              "点线",              "點線") \
  STR(kValJustify,              "两边对齐",          "兩邊對齊") \
  STR(kValLeftAlign,            "左对齐",            "左對齊") \
  STR(kValCenterAlign,          "居中",              "居中") \
  STR(kValRightAlign,           "右对齐",            "右對齊") \
  STR(kValBookStyle,            "书本样式",          "書本樣式") \
  STR(kValCompact,              "紧凑",              "緊湊") \
  STR(kValStandard,             "标准",              "標準") \
  STR(kValUpAndDown,            "上, 下",            "上, 下") \
  STR(kValDownAndUp,            "下, 上",            "下, 上") \
  STR(kValIgnore,               "忽略",              "忽略") \
  STR(kValSleep,                "休眠",              "休眠") \
  STR(kValPageTurn,             "翻页",              "翻頁") \
  STR(kValFullRefresh,          "全刷",              "全刷") \
  STR(kValConfirm,              "确认",              "確認") \
  STR(kVal1Min,                 "1分钟",             "1分鐘") \
  STR(kVal5Min,                 "5分钟",             "5分鐘") \
  STR(kVal10Min,                "10分钟",            "10分鐘") \
  STR(kVal15Min,                "15分钟",            "15分鐘") \
  STR(kVal30Min,                "30分钟",            "30分鐘") \
  STR(kValPagesFullRefresh,     "页全刷",            "頁全刷") \
  STR(kValTimes,                "倍",                "倍") \
  /* --- 蓝牙设置 --- */ \
  STR(kBluetoothTitle,          "蓝牙翻页器",        "藍牙翻頁器") \
  STR(kScanBluetooth,           "扫描蓝牙设备",      "掃描藍牙設備") \
  STR(kScanningPleaseWait,      "正在搜索附近的翻页器...", "正在搜索附近的翻頁器...") \
  STR(kFoundDevices,            "已发现 %zu 个设备",  "已發現 %zu 個設備") \
  STR(kScanningDevices,         "扫描中 - %zu个设备", "掃描中 - %zu個設備") \
  STR(kTotalDevices,            "共%zu个设备",        "共%zu個設備") \
  STR(kConnected,               "- 已连接: %s",       "- 已連接: %s") \
  STR(kScanningBluetooth,       "正在扫描蓝牙设备...", "正在掃描藍牙設備...") \
  STR(kNoBluetoothFound,        "未发现蓝牙设备",     "未發現藍牙設備") \
  STR(kPressConfirmRescan,      "按确认键重新扫描",   "按確認鍵重新掃描") \
  STR(kConnecting,              "连接中...",          "連接中...") \
  STR(kConnectSuccess,          "连接成功!",          "連接成功!") \
  STR(kDeviceLabel,             "设备: ",            "設備: ") \
  STR(kNowCanUseBT,             "现在可以使用蓝牙翻页了", "現在可以使用藍牙翻頁了") \
  STR(kConnectFailed,           "连接失败",          "連接失敗") \
  STR(kUnknownError,            "未知错误",          "未知錯誤") \
  STR(kPressAnyKeyBack,         "按任意键返回",       "按任意鍵返回") \
  STR(kEnabled,                 "已启用 (%zu 个已连接)", "已啟用 (%zu 個已連接)") \
  STR(kDisabled,                "[已禁用]",          "[已禁用]") \
  STR(kBluetoothError,          "蓝牙错误",          "藍牙錯誤") \
  STR(kDisableBluetooth,        "禁用蓝牙",          "禁用藍牙") \
  STR(kEnableBluetooth,         "启用蓝牙",          "啟用藍牙") \
  STR(kScanDevices,             "扫描设备",          "掃描設備") \
  STR(kKeyMapping,              "按键映射",          "按鍵映射") \
  STR(kBtnBack,                 "返回",              "返回") \
  STR(kBtnConfirm,              "确认",              "確認") \
  STR(kBtnLeftArrow,            "左←",               "左←") \
  STR(kBtnRightArrow,           "右→",               "右→") \
  STR(kBtnUpArrow,              "上↑",               "上↑") \
  STR(kBtnDownArrow,            "下↓",               "下↓") \
  STR(kBtnPrevPage,             "上一页 ↑",          "上一頁 ↑") \
  STR(kBtnNextPage,             "下一页 ↓",          "下一頁 ↓") \
  STR(kBtnLeft,                 "左 ←",              "左 ←") \
  STR(kBtnRight,                "右 →",              "右 →") \
  STR(kClearMapping,            "清除映射",          "清除映射") \
  STR(kBTKeyMappingTitle,       "蓝牙按键映射",      "藍牙按鍵映射") \
  STR(kUnmapped,                "未映射",            "未映射") \
  STR(kKeyMapped,               "按键%d: 0x%02X → %s", "按鍵%d: 0x%02X → %s") \
  STR(kKeyNotSet,               "按键%d: [未设置]",   "按鍵%d: [未設置]") \
  STR(kLearnKey,                "学习按键",          "學習按鍵") \
  STR(kConfigKey,               "配置按键 %d",       "配置按鍵 %d") \
  STR(kBTNotEnabled,            "未启用蓝牙",        "未啟用藍牙") \
  STR(kBTNotConnected,          "未连接蓝牙设备",    "未連接藍牙設備") \
  STR(kPleaseConnectFirst,      "请先连接设备",      "請先連接設備") \
  STR(kPressBTKey,              "请按下蓝牙设备上的按键", "請按下藍牙設備上的按鍵") \
  STR(kConnectedCanLearn,       "(已连接设备可进行学习)", "(已連接設備可進行學習)") \
  STR(kSelectFunction,          "选择功能",          "選擇功能") \
  STR(kKeyCaptured,             "按键%d: 捕获到 0x%02X", "按鍵%d: 捕獲到 0x%02X") \
  STR(kMapTo,                   "映射到:",           "映射到:") \
  /* --- OTA升级 --- */ \
  STR(kSystemUpgrade,           "系统升级",          "系統升級") \
  STR(kCheckingUpdate,          "正在检查更新...",   "正在檢查更新...") \
  STR(kAlreadyLatest,           "当前已是最新版本",  "當前已是最新版本") \
  STR(kVersionNumber,           "版本号: ",          "版本號: ") \
  STR(kLatestVersion,           "最新版本: ",        "最新版本: ") \
  STR(kCurrentVersion,          "当前版本: ",        "當前版本: ") \
  STR(kCheckFailed,             "检查更新失败",      "檢查更新失敗") \
  STR(kDownloadingFirmware,     "正在下载固件...",   "正在下載固件...") \
  STR(kDownloadFailed,          "下载失败",          "下載失敗") \
  STR(kVerifyingFirmware,       "正在校验固件...",   "正在校驗固件...") \
  STR(kVerifyFailed,            "固件校验失败",      "固件校驗失敗") \
  STR(kFirmwareIncomplete,      "下载的固件不完整，请重新下载", "下載的固件不完整，請重新下載") \
  STR(kFirmwareVerifyPass,      "固件下载完成，校验通过", "固件下載完成，校驗通過") \
  STR(kUpgradeTakesTime,        "本次升级需要1分钟左右", "本次升級需要1分鐘左右") \
  STR(kUpgradeNoResponse,       "升级过程界面可能没有响应", "升級過程界面可能沒有響應") \
  STR(kDoNotOperate,            "请勿进行其他操作", "請勿進行其他操作") \
  STR(kPressConfirmToFlash,     "  按确认键开始刷机  ", "  按確認鍵開始刷機  ") \
  STR(kInstallingFirmware,      "固件刷入中...",      "固件刷入中...") \
  STR(kInstallSuccess,          "固件安装成功！",    "固件安裝成功！") \
  STR(kInstallFailed,           "升级失败",          "升級失敗") \
  /* --- 开发者选项 (Murphy M4 本机，不走 Web API；文案须落在 m4_ui_cjk 子集) --- */ \
  STR(kDeveloperOptions,        "开发者选项",        "開發者選項") \
  STR(kUsbSerialDebug,          "USB 串口控制",      "USB 串口控制") \
  STR(kUsbSerialDebugWarn1,     "开启后，连接的主机可通过USB", "開啟後，連接的主機可通過USB") \
  STR(kUsbSerialDebugWarn2,     "可装应用、发按键、导出屏幕、读串口输出。", "可裝應用、發按鍵、導出屏幕、讀串口輸出。") \
  STR(kUsbSerialDebugWarn3,     "请在可信设备上使用，默认关。", "請在可信設備上使用，默認關。") \
  STR(kUsbSerialDebugPersist,   "开关会保持到您手动关闭。", "開關會保持到您手動關閉。") \
  /* --- 清除缓存 --- */ \
  STR(kClearCacheTitle,         "清除缓存",          "清除緩存") \
  STR(kClearCacheDesc1,         "这将清除所有缓存的书籍数据。", "這將清除所有緩存的書籍數據。") \
  STR(kClearCacheDesc2,         "所有阅读进度将丢失！", "所有閱讀進度將丟失！") \
  STR(kClearCacheDesc3,         "书籍需要重新索引",  "書籍需要重新索引") \
  STR(kClearCacheDesc4,         "才能再次打开。",    "才能再次打開。") \
  STR(kClearingCache,           "缓存清除中...",     "緩存清除中...") \
  STR(kCacheCleared,            "缓存已清除",        "緩存已清除") \
  STR(kCacheClearFailed,        "缓存清除失败",      "緩存清除失敗") \
  STR(kItemsCleared,           " 项已清除",          " 項已清除") \
  STR(kItemsClearFailed,       " 项清除失败",        " 項清除失敗") \
  STR(kCheckSerialOutput,      "请检查串口输出以获取详细信息", "請檢查串口輸出以獲取詳細信息") \
  /* --- 还原设置 --- */ \
  STR(kResetSettingsTitle,      "还原为初始设置",    "還原為初始設置") \
  STR(kResetSettingsDesc,       "这将还原所有配置项为出厂默认值。", "這將還原所有配置項為出廠默認值。") \
  STR(kResetSettingsDetails,   "包括阅读样式、字距、行距、边距等。", "包括閱讀樣式、字距、行距、邊距等。") \
  STR(kResetSettingsWarn,       "账号密码信息不受影响。", "賬號密碼信息不受影響。") \
  STR(kResetting,               "正在还原...",       "正在還原...") \
  STR(kResetSuccess,            "已还原为初始设置",  "已還原為初始設置") \
  STR(kRebootToApply,           "重启后所有设置将生效。", "重啟後所有設置將生效。") \
  STR(kConfirmReset,            "确认还原",          "確認還原") \
  /* --- 主页/文件管理 --- */ \
  STR(kNoReadingHistory,        "暂无阅读记录",      "暫無閱讀記錄") \
  STR(kBookTitle,               "书籍名称",          "書籍名稱") \
  STR(kBookAuthor,              "书籍作者",          "書籍作者") \
  STR(kBookSource,              "书籍来源",          "書籍來源") \
  STR(kBookSourceLocal,         "本地",              "本地") \
  STR(kBookSourceUnknown,       "未知来源",          "未知來源") \
  STR(kReadingProgressLabel,    "阅读进度",          "閱讀進度") \
  STR(kReadingStats,            "阅读统计",          "閱讀統計") \
  STR(kTotalReadingTime,        "累计阅读时长",      "累計閱讀時長") \
  STR(kLastReadingTime,         "上次阅读时长",      "上次閱讀時長") \
  STR(kBluetoothDisconnected,   "蓝牙已断开",        "藍牙已斷開") \
  /* --- 键盘输入 --- */ \
  STR(kEnterText,               "输入文字",          "輸入文字") \
  STR(kConnectWifi,             "连接wifi:",         "連接wifi:") \
  STR(kOrScanQRCode,            "或扫描二维码连接wifi.", "或掃描二維碼連接wifi.") \
  STR(kOpenInBrowser,           "在您的浏览器中打开此 URL", "在您的瀏覽器中打開此 URL") \
  STR(kOrScanQR,                "或使用手机扫描二维码：", "或使用手機掃描二維碼：") \
  /* --- 浏览器 --- */ \
  STR(kDataCapsule,             "数据胶囊",          "數據膠囊") \
  STR(kPleaseConfigUserPass,    "请先配置用户名/密码", "請先配置用戶名/密碼") \
  STR(kUserPassError,           "用户名或密码错误",  "用戶名或密碼錯誤") \
  STR(kAccessDenied,            "访问被拒绝，请检查用户名或密码是否正确", "訪問被拒絕，請檢查用戶名或密碼是否正確") \
  STR(kWebdavPathNotExist,      "WebDAV路径不存在",  "WebDAV路徑不存在") \
  STR(kNetworkConnectFailed,    "网络连接失败",      "網絡連接失敗") \
  STR(kConnectingTo,            "正在连接 ",         "正在連接 ") \
  /* --- 阅读器 --- */ \
  STR(kBookmarkAdded,           "书签已添加",        "書籤已添加") \
  STR(kBookmarkRemoved,         "书签已移除",        "書籤已移除") \
  STR(kEnterBookmarkNote,       "输入书签备注",      "輸入書籤備註") \
  STR(kChapterSelection,        "章节选择",          "章節選擇") \
  STR(kReaderMenu,              "阅读菜单",          "閱讀菜單") \
  /* --- X3 晃动/敲击 --- */ \
  STR(kTiltEventSwitch,         "晃动事件开关",      "晃動事件開關") \
  STR(kTiltScope,               "晃动生效范围",      "晃動生效範圍") \
  STR(kTiltScopeReaderOnly,     "仅阅读生效",        "僅閱讀生效") \
  STR(kTiltScopeGlobal,         "全局生效",          "全局生效") \
  STR(kAutoRotate,              "自动旋转屏幕",      "自動旋轉屏幕") \
  STR(kLandscapeDualPage,       "横屏双分页",        "橫屏雙分頁") \
  STR(kTapPageTurn,             "敲击翻页",          "敲擊翻頁") \
  STR(kTapAction,               "敲击动作",          "敲擊動作") \
  STR(kTapPrevPage,             "上一页",            "上一頁") \
  STR(kTapNextPage,             "下一页",            "下一頁") \
  STR(kTapThreshold,            "敲击灵敏度",        "敲擊靈敏度") \
  STR(kTapSensLow,              "低(0.35g)",         "低(0.35g)") \
  STR(kTapSensMedLow,           "中低(0.25g)",       "中低(0.25g)") \
  STR(kTapSensMed,              "中(0.18g)",         "中(0.18g)") \
  STR(kTapSensMedHigh,          "中高(0.12g)",       "中高(0.12g)") \
  STR(kTapSensHigh,             "高(0.08g)",         "高(0.08g)") \
  STR(kTapCooldown,             "敲击冷却时间",      "敲擊冷卻時間") \
  STR(kTapCd300,                "300ms",             "300ms") \
  STR(kTapCd400,                "400ms",             "400ms") \
  STR(kTapCd600,                "600ms",             "600ms") \
  STR(kTapCd800,                "800ms",             "800ms") \
  STR(kTapCd1000,               "1000ms",            "1000ms") \
  /* --- 坚果云同步 --- */ \
  STR(kJianGuoSyncTitle,        "坚果云同步",        "堅果雲同步") \
  STR(kJianGuoAccount,          "坚果云账号",        "堅果雲賬號") \
  STR(kJianGuoAppPassword,      "坚果云应用密码",    "堅果雲應用密碼") \
  STR(kJianGuoReadFolder,       "坚果云读取目录",    "堅果雲讀取目錄") \
  /* --- 数据胶囊设置 --- */ \
  STR(kDataCapsuleUsername,     "数据胶囊用户名",    "數據膠囊用戶名") \
  STR(kDataCapsulePassword,     "数据胶囊密码",      "數據膠囊密碼") \
  STR(kNotConfigured,          "[未配置]",          "[未配置]") \
  STR(kConfigured,             "[已配置]",          "[已配置]") \
  STR(kAppPassword,            "应用密码",          "應用密碼") \
  STR(kEnterJianGuoAccount,    "输入坚果云账号",     "輸入堅果雲賬號") \
  STR(kEnterAppPassword,       "输入应用密码",       "輸入應用密碼") \
  STR(kDataCapsuleUsername2,   "用户名",            "用戶名") \
  STR(kPassword,               "密码",              "密碼") \
  STR(kWebdavUrl,              "WebDAV地址",        "WebDAV地址") \
  STR(kEnterUsername,          "输入用户名",        "輸入用戶名") \
  STR(kEnterPassword,          "输入密码",          "輸入密碼") \
  STR(kEnterWebdavUrl,         "输入WebDAV地址",    "輸入WebDAV地址") \
  STR(kNotSet,                 "[未设置]",          "[未設置]") \
  STR(kHasBeenSet,             "[已设置]",          "[已設置]") \
  STR(kDefault,                "[默认]",            "[默認]") \
  STR(kCustom,                 "[自定义]",          "[自定義]") \
  STR(kPleaseSetCredentials,   "[请先设置凭据]",     "[請先設置憑據]") \
  STR(kLoadingFontPleaseWait,  "正在加载字体，请稍候...", "正在載入字體，請稍候...") \
  STR(kFontLoadFailed,        "字体加载失败，保持原字体", "字體載入失敗，保持原字體") \
  STR(kSelectFont,             "选择字体",          "選擇字體") \
  STR(kPleaseVisit,            "请访问",            "請訪問") \
  STR(kGenerateEpdFont,        "生成.epdfont格式的字体", "生成.epdfont格式的字體") \
  STR(kCopyToFontsDir,         "拷贝到内存卡/FONT目录下", "拷貝到內存卡/FONT目錄下") \
  STR(kConfirmShort,           "确定",              "確定") \
  STR(kConfigCalibreOpds,      "配置calibre的OPDS服务器地址和认证信息", "配置calibre的OPDS服務器地址和認證信息") \
  STR(kCompleted,             "已完成",            "已完成") \
  /* --- WiFi --- */ \
  STR(kSelectWifi,              "选择WiFi网络",      "選擇WiFi網絡") \
  STR(kScanningWifi,            "正在扫描WiFi...",   "正在掃描WiFi...") \
  STR(kNoWifiFound,             "未找到WiFi网络",    "未找到WiFi網絡") \
  STR(kEnterWifiPassword,       "输入WiFi密码",      "輸入WiFi密碼") \
  STR(kWifiConnecting,          "正在连接WiFi...",   "正在連接WiFi...") \
  STR(kWifiConnectSuccess,      "WiFi连接成功",      "WiFi連接成功") \
  STR(kWifiConnectFailed,       "WiFi连接失败",      "WiFi連接失敗") \
  /* --- 其他 --- */ \
  STR(kFontSelection,           "字体选择",          "字體選擇") \
  STR(kKOReaderSync,            "KOReader 同步",     "KOReader 同步") \
  STR(kOPDSBrowser,             "OPDS 浏览器",       "OPDS 瀏覽器") \
  STR(kRemapFrontButtons,       "重新映射前置按键",    "重新映射前置按鍵") \
  STR(kPressFrontBtnForRole,    "请为每个功能按下前置按键", "請為每個功能按下前置按鍵") \
  STR(kUnassigned,              "未分配",              "未分配") \
  STR(kAlreadyAssigned,         "已分配",              "已分配") \
  STR(kSideBtnUpReset,          "上侧键: 恢复默认布局", "上側鍵: 恢復默認佈局") \
  STR(kSideBtnDownCancel,       "下侧键: 取消重映射",   "下側鍵: 取消重映射") \
  STR(kBtn1st,                  "第1个按键",           "第1個按鍵") \
  STR(kBtn2nd,                  "第2个按键",           "第2個按鍵") \
  STR(kBtn3rd,                  "第3个按键",           "第3個按鍵") \
  STR(kBtn4th,                  "第4个按键",           "第4個按鍵") \
  STR(kUnknown,                 "未知",                "未知") \
  /* --- 主页菜单 --- */ \
  STR(kFileManager,             "文件管理",            "文件管理") \
  STR(kReadingHistory,          "阅读历史",            "閱讀歷史") \
  STR(kJianGuoDisk,             "坚果云盘",            "堅果雲盤") \
  STR(kBookmarkNotes,           "书签笔记",            "書籤筆記") \
  STR(kNetworkManage,           "网络管理",            "網絡管理") \
  /* --- 内存警告 --- */ \
  STR(kMemLowWarning,           "设备内存不足，可能连接过网络或蓝牙", "設備內存不足，可能連接過網絡或藍牙") \
  STR(kMemLowReboot,            "是否立即重启恢复？",      "是否立即重啟恢復？") \
  STR(kRebootNow,               "[立即重启]",             "[立即重啟]") \
  /* --- 数字选择 --- */ \
  STR(kNumberSelectionTitle,    "数值选择",          "數值選擇") \
  /* --- 阅读器弹出提示 --- */ \
  STR(kGlobalNextPageModeOn,    "已进入全局下一页模式", "已進入全局下一頁模式") \
  STR(kGlobalNextPageModeOff,   "已退出全局下一页模式", "已退出全局下一頁模式") \
  STR(kBTDeviceConnected,      "蓝牙设备已连接",     "藍牙設備已連接") \
  STR(kBTConnectingEllipsis,   "连接蓝牙中...",      "連接藍牙中...") \
  STR(kBTConnected,            "蓝牙已连接",         "藍牙已連接") \
  STR(kBTConnectFailed,        "蓝牙连接失败",       "藍牙連接失敗") \
  STR(kBTClosed,               "蓝牙已关闭",         "藍牙已關閉") \
  STR(kBTError,                "蓝牙操作异常",       "藍牙操作異常") \
  STR(kAutoPageTurnOn,         "自动翻页已开启",     "自動翻頁已開啟") \
  STR(kAutoPageTurnOff,        "自动翻页已关闭",     "自動翻頁已關閉") \
  STR(kAntiAliasingOn,         "抗锯齿已开启",       "抗鋸齒已開啟") \
  STR(kAntiAliasingOff,        "抗锯齿已关闭",       "抗鋸齒已關閉") \
  STR(kDarkModeOn,             "暗黑模式已开启",     "暗黑模式已開啟") \
  STR(kDarkModeOff,            "暗黑模式已关闭",     "暗黑模式已關閉") \
  STR(kTiltPageTurnOn,         "晃动翻页已开启",     "晃動翻頁已開啟") \
  STR(kTiltPageTurnOff,        "晃动翻页已关闭",     "晃動翻頁已關閉") \
  STR(kSwitchToPortrait,       "切换为按钮在下侧竖屏", "切換為按鈕在下側豎屏") \
  STR(kSwitchToLandscape,      "切换为按钮在右侧横屏", "切換為按鈕在右側橫屏") \
  STR(kTxtSyncNotSupported,    "TXT暂不支持同步",    "TXT暫不支持同步") \
  /* --- OTA在线升级 --- */ \
  STR(kUpgradingFirmware,      "正在升级固件，请勿关机或操作设备...", "正在升級固件，請勿關機或操作設備...") \
  STR(kInsufficientMemory,     "可用内存不足(",       "可用內存不足(") \
  STR(kNetworkRequestFailed,   "网络请求失败",       "網絡請求失敗") \
  STR(kVersionParseFailed,     "版本信息解析失败",   "版本信息解析失敗") \
  STR(kWifiDisconnected,       "WiFi连接已断开",     "WiFi連接已斷開") \
  STR(kFileWriteFailed,        "文件写入失败",       "文件寫入失敗") \
  STR(kFlashingError,          "刷机过程出错",       "刷機過程出錯") \
  STR(kUpgradeComplete,        "升级完成",           "升級完成") \
  /* --- 浏览器（坚果云/数据胶囊） --- */ \
  STR(kCheckWifi,              "检查WiFi...",        "檢查WiFi...") \
  STR(kJianGuo,                "坚果云",             "堅果雲") \
  STR(kRetry,                  "重试",               "重試") \
  STR(kDownloadFailedShort,    "下载失败",           "下載失敗") \
  /* --- 网络功能 --- */ \
  STR(kFileTransfer,           "文件传输",           "文件傳輸") \
  STR(kWifiName,               "wifi名称: ",         "wifi名稱: ") \
  STR(kConnectPhoneToHotspot,  "打开手机wifi连接到CrossLink热点", "打開手機wifi連接到CrossLink熱點") \
  STR(kOrScanQRConnectHotspot, "或扫描二维码连接到CrossLink热点", "或掃描二維碼連接到CrossLink熱點") \
  STR(kOpenInPhoneBrowser,     "用手机浏览器打开以下地址：", "用手機瀏覽器打開以下地址：") \
  STR(kOrScanQRFileManager,    "或扫描二维码打开文件管理页面", "或掃描二維碼打開文件管理頁面") \
  STR(kWifiFunctionSettings,   "wifi功能设置",       "wifi功能設置") \
  STR(kHowToConnect,           "你想如何连接?",      "你想如何連接?") \
  STR(kMode1,                  "1） 手机连接到设备传书（推荐）", "1） 手機連接到設備傳書（推薦）") \
  STR(kMode2,                  "2） 设备连接到WiFi传书", "2） 設備連接到WiFi傳書") \
  STR(kMode3,                  "3） 使用Calibre无线设备传输", "3） 使用Calibre無線設備傳輸") \
  STR(kContinue,               "继续",               "繼續") \
  STR(kLeftArrow,              "向左",               "向左") \
  STR(kRightArrow,             "向右",               "向右") \
  /* --- 关机/休眠 --- */ \
  STR(kShuttingDown,           "关机中",             "關機中") \
  STR(kSleeping,               "已休眠",             "已休眠") \
  STR(kPoweredOff,             "已关机",             "已關機") \
  /* --- 阅读统计时间单位 --- */ \
  STR(kDayUnit,                "天",                 "天") \
  STR(kHourUnit,               "小时",               "小時") \
  STR(kMinuteUnit,             "分钟",               "分鐘") \
  STR(kSecondUnit,             "秒",                 "秒") \
  STR(kZeroMinutes,            "0分钟",              "0分鐘") \
  /* --- 翻页状态提示 --- */ \
  STR(kHalfScreenPaging,       "半屏翻页中↓",        "半屏翻頁中↓") \
  STR(kAutoPaging,             "自动翻页中↓",        "自動翻頁中↓") \
  /* --- 主页格言 --- */ \
  STR(kQuote1,  "世上无难事，只怕有心人。", "世上無難事，只怕有心人。") \
  STR(kQuote2,  "学而不厌，诲人不倦。",     "學而不厭，誨人不倦。") \
  STR(kQuote3,  "天道酬勤，厚德载物。",     "天道酬勤，厚德載物。") \
  STR(kQuote4,  "宁静致远，淡泊明志。",     "寧靜致遠，淡泊明志。") \
  STR(kQuote5,  "知行合一，止于至善。",     "知行合一，止於至善。") \
  STR(kQuote6,  "锲而不舍，金石可镂。",     "鍥而不捨，金石可鏤。") \
  STR(kQuote7,  "海纳百川，有容乃大。",     "海納百川，有容乃大。") \
  STR(kQuote8,  "敏而好学，不耻下问。",     "敏而好學，不恥下問。") \
  STR(kQuote9,  "温故知新，可以为师。",     "溫故知新，可以為師。") \
  STR(kQuote10, "己所不欲，勿施于人。",     "己所不欲，勿施於人。") \
  STR(kQuote11, "三人行，必有我师。",       "三人行，必有我師。") \
  STR(kQuote12, "读书破万卷，下笔如有神。", "讀書破萬卷，下筆如有神。") \
  STR(kQuote13, "欲穷千里目，更上一层楼。", "欲窮千里目，更上一層樓。") \
  STR(kQuote14, "会当凌绝顶，一览众山小。", "會當凌絕頂，一覽眾山小。") \
  STR(kQuote15, "少壮不努力，老大徒伤悲。", "少壯不努力，老大徒傷悲。") \
  /* --- 向上/向下(完整) --- */ \
  STR(kMoveUp,                 "向上",                "向上") \
  STR(kMoveDown,               "向下",                "向下")

// ═══════════════════════════════════════════════════════════════
//  自动生成枚举
// ═══════════════════════════════════════════════════════════════
enum class Str : uint16_t {
  #define STR(key, zh_cn, zh_tw) key,
  STR_TABLE
  #undef STR
  COUNT
};

// ═══════════════════════════════════════════════════════════════
//  自动生成字符串数组
// ═══════════════════════════════════════════════════════════════
static const char* const STRINGS_ZH_CN[] = {
  #define STR(key, zh_cn, zh_tw) zh_cn,
  STR_TABLE
  #undef STR
};

static const char* const STRINGS_ZH_TW[] = {
  #define STR(key, zh_cn, zh_tw) zh_tw,
  STR_TABLE
  #undef STR
};

// ═══════════════════════════════════════════════════════════════
//  翻译函数
// ═══════════════════════════════════════════════════════════════
inline const char* L(Str key) {
  const uint16_t idx = static_cast<uint16_t>(key);
  if (idx >= static_cast<uint16_t>(Str::COUNT)) return "";
  // systemLanguage: 0=简体中文, 1=繁体中文
  return SETTINGS.systemLanguage == 0 ? STRINGS_ZH_CN[idx] : STRINGS_ZH_TW[idx];
}

// 清理 X-macro 污染
#undef STR
