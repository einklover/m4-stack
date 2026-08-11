#ifndef DATACAPSULEBROWSERACTIVITY_H
#define DATACAPSULEBROWSERACTIVITY_H

#include "../ActivityWithSubactivity.h"
#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include <vector>
#include <string>
#include <WiFiClientSecure.h>

// WebDAV Entry for Data Capsule
struct DataCapsuleEntry {
    std::string title;        // 文件名/文件夹名（显示用）
    std::string path;         // 相对路径（显示用）
    std::string encodedPath;  // URL编码后的路径（用于下载请求）
    uint32_t fileSize;        // 文件大小（字节），0表示文件夹
    enum Type { FOLDER, BOOK_FILE } type;
};

class DataCapsuleBrowserActivity : public ActivityWithSubactivity {
public:
    enum class BrowserState {
        CHECK_WIFI,
        WIFI_SELECTION,
        LOADING,
        BROWSING,
        DOWNLOADING,
        ERROR
    };

    DataCapsuleBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                               const std::function<void()>& onGoHome)
      : ActivityWithSubactivity("DataCapsuleBrowser", renderer, mappedInput), onGoHome(onGoHome) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

private:
    static void taskTrampoline(void* param);
    void displayTaskLoop();
    void render() const;
    void fetchFeed(const std::string& path);
    void navigateToEntry(const DataCapsuleEntry& entry);
    void navigateBack();
    void downloadBook(const DataCapsuleEntry& book);
    void checkAndConnectWifi();
    void checkAutoConnectProgress();
    void launchWifiSelection();
    void onWifiSelectionComplete(const bool connected);
    void parseXmlEntries(const String& xmlResult, 
                         const std::string& basePath,
                         const String& urlBasePath,
                         std::vector<DataCapsuleEntry>& outEntries);

    static bool endsWith(const std::string& str, const std::string& suffix);
    void drawLocalButtonHints(const char* btn1, const char* btn2, const char* btn3, const char* btn4) const;

    const std::function<void()> onGoHome;

    // 成员变量声明
    static std::vector<DataCapsuleEntry> entries;
    static std::string currentPath;
    static std::vector<std::string> navigationHistory;
    static int selectorIndex;
    static std::string errorMessage;
    static std::string statusMessage;
    size_t downloadProgress;
    size_t downloadTotal;
    static BrowserState state;
    static SemaphoreHandle_t renderingMutex;
    static TaskHandle_t displayTaskHandle;
    static bool updateRequired;

    // 自动连接WiFi相关
    bool autoConnectAttempted = false;
    unsigned long autoConnectStartTime = 0;

    // 标记是否刚进入Activity（用于吸收进入时的按钮释放事件）
    bool justEntered = false;
};

#endif // DATACAPSULEBROWSERACTIVITY_H
