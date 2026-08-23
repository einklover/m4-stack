#pragma once
#include <cstdint>
#include <iosfwd>

class CrossPointSettings {
 private:
  // Private constructor for singleton
  CrossPointSettings() = default;

  // Static instance
  static CrossPointSettings instance;

  // Internal: load from legacy binary format (one-time migration only)
  bool loadFromBinaryFile();

 public:
  // Delete copy constructor and assignment
  CrossPointSettings(const CrossPointSettings&) = delete;
  CrossPointSettings& operator=(const CrossPointSettings&) = delete;

  enum SLEEP_SCREEN_MODE {
    DARK = 0,
    LIGHT = 1,
    CUSTOM = 2,
    COVER = 3,
    MARSK = 4,
    MARSK2 = 5,
    BLANK = 6,
    COVER_CUSTOM = 7,
    TRANSPARENT = 8,  // 透明叠加：保留当前 framebuffer（阅读内容），叠加覆盖层图片
    SLEEP_SCREEN_MODE_COUNT
  };
  enum SLEEP_SCREEN_COVER_MODE { FIT = 0, CROP = 1, SLEEP_SCREEN_COVER_MODE_COUNT };
  enum SLEEP_SCREEN_COVER_FILTER {
    NO_FILTER = 0,
    BLACK_AND_WHITE = 1,
    INVERTED_BLACK_AND_WHITE = 2,
    SLEEP_SCREEN_COVER_FILTER_COUNT
  };

  // Status bar display type enum
  enum STATUS_BAR_MODE {
    NONE = 0,
    NO_PROGRESS = 1,
    FULL = 2,
    BOOK_PROGRESS_BAR = 3,
    ONLY_BOOK_PROGRESS_BAR = 4,
    CHAPTER_PROGRESS_BAR = 5,
    STATUS_BAR_MODE_COUNT
  };

  enum ORIENTATION {
    PORTRAIT = 0,       // 480x800 logical coordinates (current default)
    LANDSCAPE_CW = 1,   // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    INVERTED = 2,       // 480x800 logical coordinates, inverted
    LANDSCAPE_CCW = 3,  // 800x480 logical coordinates, native panel orientation
    ORIENTATION_COUNT
  };

  // Front button layout options (legacy)
  // Default: Back, Confirm, Left, Right
  // Swapped: Left, Right, Back, Confirm
  enum FRONT_BUTTON_LAYOUT {
    BACK_CONFIRM_LEFT_RIGHT = 0,
    LEFT_RIGHT_BACK_CONFIRM = 1,
    LEFT_BACK_CONFIRM_RIGHT = 2,
    BACK_CONFIRM_RIGHT_LEFT = 3,
    FRONT_BUTTON_LAYOUT_COUNT
  };

  // Front button hardware identifiers (for remapping)
  enum FRONT_BUTTON_HARDWARE {
    FRONT_HW_BACK = 0,
    FRONT_HW_CONFIRM = 1,
    FRONT_HW_LEFT = 2,
    FRONT_HW_RIGHT = 3,
    FRONT_BUTTON_HARDWARE_COUNT
  };

  // Side button layout options
  // Default: Previous, Next
  // Swapped: Next, Previous
  enum SIDE_BUTTON_LAYOUT { PREV_NEXT = 0, NEXT_PREV = 1, SIDE_BUTTON_LAYOUT_COUNT };

  // 主页面图标风格选项
  enum HOME_ICON_STYLE {
    CORNERS_ONLY = 0,           // 仅四角
    GRAY_BG_ONLY = 1,           // 仅灰色背景
    DASHED_BORDER_ONLY = 2,     // 仅虚线边框
    SOLID_BORDER_ONLY = 3,      // 仅实线边框
    CORNERS_AND_GRAY_BG = 4,    // 四角+灰色背景
    HOME_ICON_STYLE_COUNT
  };

  // Font family options
  enum FONT_FAMILY { SYSTEM_FONT = 0, FONT_CUSTOM = 1, FONT_FAMILY_COUNT };
  // Font size options
  enum FONT_SIZE { SMALL = 0, MEDIUM = 1, LARGE = 2, EXTRA_LARGE = 3, FONT_SIZE_COUNT };
  // Reader body size is one family-independent pixel setting. The old enum is
  // retained only for binary/settings migration compatibility.
  static constexpr uint8_t READER_PIXEL_SIZE_MIN = 12;
  static constexpr uint8_t READER_PIXEL_SIZE_MAX = 48;
  static uint8_t clampReaderPixelSize(int px) {
    if (px < READER_PIXEL_SIZE_MIN) return READER_PIXEL_SIZE_MIN;
    if (px > READER_PIXEL_SIZE_MAX) return READER_PIXEL_SIZE_MAX;
    return static_cast<uint8_t>(px);
  }
  enum LINE_COMPRESSION { TIGHT = 0, NORMAL = 1, WIDE = 2, LINE_COMPRESSION_COUNT };
  enum WORDS_COMPRESSION { WORD_TIGHT = 0, WORD_NORMAL = 1, WORD_WIDE = 2, WORD_COMPRESSION_COUNT };
  enum PARAGRAPH_ALIGNMENT {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
    BOOK_STYLE = 4,
    PARAGRAPH_ALIGNMENT_COUNT
  };

  // Auto-sleep timeout options (in minutes)
  enum SLEEP_TIMEOUT {
    SLEEP_1_MIN = 0,
    SLEEP_5_MIN = 1,
    SLEEP_10_MIN = 2,
    SLEEP_15_MIN = 3,
    SLEEP_30_MIN = 4,
    SLEEP_TIMEOUT_COUNT
  };

  // E-ink cleanup frequency (pages between reader-body cleanup passes)
  enum REFRESH_FREQUENCY {
    REFRESH_1 = 0,
    REFRESH_5 = 1,
    REFRESH_10 = 2,
    REFRESH_15 = 3,
    REFRESH_30 = 4,
    REFRESH_FREQUENCY_COUNT
  };

  // Short power button press actions
  enum SHORT_PWRBTN { IGNORE = 0, SLEEP = 1, PAGE_TURN = 2, FULL_REFRESH = 3, CONFIRM = 4, SHORT_PWRBTN_COUNT };

  // Hide battery percentage
  enum HIDE_BATTERY_PERCENTAGE { HIDE_NEVER = 0, HIDE_READER = 1, HIDE_ALWAYS = 2, HIDE_BATTERY_PERCENTAGE_COUNT };

  // UI Theme
  enum UI_THEME { CLASSIC = 0, LYRA = 1 };

  // Chinese punctuation width mode
  enum PUNCT_WIDTH { PUNCT_COMPACT = 0, PUNCT_STANDARD = 1 };

  // Sleep screen settings
  uint8_t sleepScreen = LIGHT;
  // Sleep screen cover mode settings
  uint8_t sleepScreenCoverMode = FIT;
  // Sleep screen cover filter
  uint8_t sleepScreenCoverFilter = NO_FILTER;
  // Status bar settings
  uint8_t statusBar = FULL;
  // Text rendering settings
  uint8_t extraParagraphSpacing = 0;
  uint8_t textAntiAliasing = 0;
  // Short power button click behaviour
  uint8_t shortPwrBtn = PAGE_TURN;
  // EPUB reading orientation settings
  // 0 = portrait (default), 1 = landscape clockwise, 2 = inverted, 3 = landscape counter-clockwise
  uint8_t orientation = PORTRAIT;
  // Button layouts (front layout retained for migration only)
  uint8_t frontButtonLayout = BACK_CONFIRM_LEFT_RIGHT;
  uint8_t sideButtonLayout = PREV_NEXT;
  // Front button remap (logical -> hardware)
  // Used by MappedInputManager to translate logical buttons into physical front buttons.
  uint8_t frontButtonBack = FRONT_HW_BACK;
  uint8_t frontButtonConfirm = FRONT_HW_CONFIRM;
  uint8_t frontButtonLeft = FRONT_HW_LEFT;
  uint8_t frontButtonRight = FRONT_HW_RIGHT;
  // Reader font settings
  uint8_t fontFamily = SYSTEM_FONT;
  uint8_t readerPixelSize = 18;  // canonical reader body size, independent of family
  uint8_t customFontSize = 0;  // legacy JSON/binary alias; runtime never uses this
  char customFontFamily[64] = "";
  uint8_t fontSize = LARGE;
  uint8_t lineSpacing = NORMAL;   // Legacy: 0=TIGHT, 1=NORMAL, 2=WIDE
  uint8_t customLineSpacing = 10;  // Custom line spacing: 5-20 -> 0.5-2.0 (default 1.1)
  uint8_t firstlineintented = 1;
  int8_t wordSpacing = 0;
  int8_t underlineOffset = 5;  // Underline offset from baseline in pixels: -10 to 10 (default 0)
  uint8_t underlineStyle = 1;  // Underline style: 0=Solid, 1=ShortDash, 2=MediumDash, 3=LongDash, 4=DotLine (default 1=ShortDash)
  uint8_t paragraphAlignment = JUSTIFIED;
  // Chinese punctuation width: 0=compact (default), 1=standard (full-width)
  uint8_t chinesePunctWidth = PUNCT_COMPACT;

  // Show images in EPUB: 1=enabled (default), 0=disabled
  uint8_t epubShowImages = 1;
  // Auto-sleep timeout setting (default 10 minutes)
  uint8_t sleepTimeout = SLEEP_10_MIN;
  // E-ink refresh frequency (default 15 pages)
  // NOTE: Now stores actual page count (1-30), not enum index
  uint8_t refreshFrequency = 10;
  uint8_t hyphenationEnabled = 0;

  // Reader screen margin settings
  // Reader screen margin settings
  uint8_t screenMargin_Top = 10;
  uint8_t screenMargin_Left = 10;
  uint8_t screenMargin_Bottom = 10;
  uint8_t screenMargin_Right = 10;
  // OPDS browser settings
  char opdsServerUrl[128] = "";
  char opdsUsername[64] = "";
  char opdsPassword[64] = "";

  // 坚果云 settings
  char jgUsername[64] = "";     // 坚果云账号（邮箱）
  char jgAppPassword[64] = "";  // 坚果云应用密码
  char jgBookFolder[128] = "";  // 电子书文件夹路径

  // 数据胶囊 settings (WebDAV)
  char dcUsername[64] = "";     // 数据胶囊用户名
  char dcPassword[64] = "";     // 数据胶囊密码
  char dcWebdavUrl[128] = "https://data.cstcloud.cn/dav";  // 数据胶囊WebDAV地址

  // Z-Library 配置
  char zlibEmail[64] = "";
  char zlibPassword[64] = "";
  // 新加划线
  uint8_t extraline = 1;
  // Bluetooth enabled state (persistent)
  uint8_t bluetoothEnabled = 0;

  // 阅读背景设置
  uint8_t ReadingScreenEnabled = 0;  // Whether to show the reading screen background image
  // Hide battery percentage
  uint8_t hideBatteryPercentage = HIDE_ALWAYS;
  // Long press to boot (1=require 2s hold to wake from sleep, 0=short press ok)
  uint8_t longPressBoot = 1;
  // Never trigger reader-body cleanup (1=disabled cleanup, 0=use refreshFrequency)
  uint8_t neverFullRefresh = 0;

  // Long-press chapter skip on side buttons
  uint8_t longPressChapterSkip = 1;
  // Global next page mode enabled (persistent)
  uint8_t globalNextPageModeEnabled = 0;
  // UI Theme
  uint8_t uiTheme = LYRA;
  // Sunlight fading compensation
  uint8_t fadingFix = 0;
  // Use book's embedded CSS styles for EPUB rendering (1 = enabled, 0 = disabled)
  uint8_t embeddedStyle = 0;
  // Library home directory (file manager entry point)
  char libraryHomePath[128] = "";

  // Auto page turn settings
  uint8_t autoPageTurnEnabled = 0;    // 0=关闭, 1=开启
  uint8_t autoPageTurnMode = 1;       // 0=全屏翻页, 1=半屏翻页
  uint8_t autoPageTurnInterval = 10;  // 翻页间隔(秒), 1-60

  // Tilt page turn settings (晃动翻页)
  uint8_t tiltPageTurnEnabled = 0;    // 0=关闭, 1=开启
  uint8_t tiltScope = 0;              // 0=仅阅读生效, 1=全局生效
  uint8_t tiltTriggerAngle = 20;      // 触发角度(度), 1-90
  uint8_t tiltReleaseAngle = 10;       // 回正角度(度), 0-89, 默认=triggerAngle-1
  uint16_t tiltHoldTimeMs = 200;      // 倾斜持续时间(毫秒), 50-2000
  uint16_t tiltCooldownTimeMs = 800;  // 冷却时间(毫秒), 100-5000
  uint8_t tiltLeftAction = 0;         // 向左倾斜: 0=上一页, 1=下一页
  uint8_t tiltRightAction = 1;        // 向右倾斜: 0=上一页, 1=下一页
  uint8_t tiltLargeLeftAction = 0;    // 大幅度向左倾斜(>70°): 0=上一页, 1=下一页, 2=自动翻页, 3=打开菜单
  uint8_t tiltLargeRightAction = 1;   // 大幅度向右倾斜(>70°): 0=上一页, 1=下一页, 2=自动翻页, 3=打开菜单

  // Tap page turn settings (敲击翻页)
  uint8_t tapPageTurnEnabled = 0;     // 0=关闭, 1=开启
  uint8_t tapAction = 1;              // 敲击动作: 0=上一页, 1=下一页
  uint8_t tapThresholdG = 12;         // 触发阈值(0.01g单位), 5-80, 默认12=0.12g
  uint16_t tapCooldownMs = 600;       // 冷却时间(毫秒), 200-3000

  // Auto-rotate settings (自动旋转)
  uint8_t autoRotateEnabled = 0;      // 0=关闭, 1=开启

  // Landscape dual-page mode (横屏双分页)
  uint8_t landscapeDualPageEnabled = 0;  // 0=关闭（默认，横屏时文字不变左右模式）, 1=开启

  // Button hints display toggle (按钮提示开关)
  uint8_t buttonHintsEnabled = 0;  // 0=隐藏按钮提示（默认）, 1=显示

  // Frontlight (Murphy M4 dual-channel cool/warm PWM). 0 brightness = off.
  // Warmth 0 = fully cool, 100 = fully warm. Retained across reboot.
  uint8_t frontlightBrightness = 20;
  uint8_t frontlightWarmth = 50;

  // WiFi每次都重新选择 (1=每次都重新选择, 0=自动重连上次WiFi)
  uint8_t wifiAlwaysReselect = 1;

  // PNG/JPG 关机壁纸渲染模式：1=普通壁纸(invertScreen→深色背景), 0=透明壁纸(浅色背景)
  uint8_t sleepPngInvert = 1;

  // 关机前全刷：1=每次关机前清屏全刷，0=直接关机不清屏
  uint8_t sleepBeforeFullRefresh = 1;

  // --- 系统动画（Home / activity 转场）---
  // 1=开启系统界面擦入动画，0=关闭。与阅读器翻页动画独立。
  uint8_t systemAnimationEnabled = 0;

  // --- 翻页动画（阅读器 PageTurnAnimation）---
  // 阅读器开关：1=开启翻页动画，0=关闭（默认关闭，传统瞬时翻页）
  uint8_t pageTurnAnimationEnabled = 0;
  // 动画步数（2..64，默认 9；= 刷新次数）
  uint8_t pageTurnAnimationSteps = 9;
  // 局部窗口宽度（步长倍数 1..16，默认 4）
  uint8_t pageTurnAnimationMult = 4;
  // 波形 TP 值（0x01..0x10，默认 0x02）
  uint8_t pageTurnAnimationTp = 0x02;
  // 帧率（0x22/0x44/0x88，默认 0x88）
  uint8_t pageTurnAnimationFrameRate = 0x88;
  // 翻页方向：0=右→左，1=左→右，2=下→上，3=上→下（默认 0）
  uint8_t pageTurnAnimationDir = 0;

  // 图片渲染质量：0=普通(1-bit BW+dithering)，1=高清(2-bit 4阶灰度)
  enum IMAGE_QUALITY { QUALITY_FAST = 0, QUALITY_NORMAL = 1, QUALITY_HD = 2 };
  uint8_t imageQuality = QUALITY_NORMAL;

  // 透明叠加壁纸：覆盖层图片的 _hd.pxc 路径（用于 TRANSPARENT 关机模式）
  char transparentOverlayPxc[128] = "";

  // EPUB 阅读暗黑模式（反色显示，仅阅读页面生效）
  uint8_t epubDarkMode = 0;

  // 主页面图标风格
  uint8_t homeIconStyle = CORNERS_ONLY;

  // 图标风格：0=风格一, 1=风格二, 2=风格三
  uint8_t iconStyle = 0;

  // 透明壁纸去白色：1=白色像素作为透明扔掉，0=保留白色像素
  uint8_t transparentRemoveWhite = 1;

  // 长按确认键功能映射（0=切换全局下一页, 1=打开蓝牙配置, 2=切换自动翻页, 3=切换抗锯齿, 4=切换暗黑模式, X3:5=切换晃动翻页, 5/6=无）
#ifdef CROSSPOINT_X3
  uint8_t longPressConfirmAction = 6;  // 默认无
#else
  uint8_t longPressConfirmAction = 5;  // 默认无
#endif

  // 文件管理器确认键行为：0=单击弹出菜单（默认），1=长按弹出菜单
  uint8_t libraryLongPressMenu = 1;

  // 开机自动同步时间（X4 NTP对时开关，默认关闭）
  uint8_t autoSyncTimeOnBoot = 0;

  // 阅读页面显示时间而非章节名（1=显示时间，0=显示章节名，默认开启）
  uint8_t showTimeInsteadOfChapter = 1;

  // 蓝牙按键自定义映射（6个槽）
  // Code: BT HID按键码 (0x00=未设置)；Action: 目标按键索引 (0xFF=未映射, 对应HalGPIO::BTN_*)
  // BTN_BACK=0, BTN_CONFIRM=1, BTN_LEFT=2, BTN_RIGHT=3, BTN_UP=4, BTN_DOWN=5
  uint8_t btKey1Code = 0x00;   uint8_t btKey1Action = 0xFF;
  uint8_t btKey2Code = 0x00;   uint8_t btKey2Action = 0xFF;
  uint8_t btKey3Code = 0x00;   uint8_t btKey3Action = 0xFF;
  uint8_t btKey4Code = 0x00;   uint8_t btKey4Action = 0xFF;
  uint8_t btKey5Code = 0x00;   uint8_t btKey5Action = 0xFF;
  uint8_t btKey6Code = 0x00;   uint8_t btKey6Action = 0xFF;

  // 直读TXT文档：1=直接读取TXT（默认），0=转换为EPUB后打开
  uint8_t directTxtRead = 1;

  // 系统语言：0=简体中文（默认），1=繁体中文
  uint8_t systemLanguage = 0;

  // Murphy M4 only: USB serial debug bridge (m4adb). 0=off (default), 1=on.
  // Changed only via on-device Developer Options UI — never via web/API/serial.
  // Persists across reboot; runtime-gated (bridge is compiled into M4 firmware).
  uint8_t developerSerialDebugEnabled = 0;

  ~CrossPointSettings() = default;

  uint8_t getReaderPixelSize() const { return clampReaderPixelSize(readerPixelSize); }
  void setReaderPixelSize(uint8_t px) { readerPixelSize = clampReaderPixelSize(px); }

  // Get singleton instance
  static CrossPointSettings& getInstance() { return instance; }

  uint16_t getPowerButtonDuration() const {
    return (shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP) ? 10 : 400;
  }
  int getReaderFontId() const;

  bool saveToFile() const;
  bool loadFromFile();
  void resetToDefaults();

  float getReaderLineCompression() const;
  unsigned long getSleepTimeoutMs() const;
  int getRefreshFrequency() const;
};

// Helper macro to access settings
#define SETTINGS CrossPointSettings::getInstance()
