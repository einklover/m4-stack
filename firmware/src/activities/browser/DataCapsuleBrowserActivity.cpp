#include "DataCapsuleBrowserActivity.h"

#include <GfxRenderer.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <base64.h>
#include <string>
#include <algorithm>

#include "CrossPointSettings.h"
#include "I18n.h"
#include "MappedInputManager.h"
#include "WifiCredentialStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"
#include "network/HttpDownloader.h"
#include "util/M4ListTouchPolicy.h"
#include "util/StringUtils.h"
#include <SDCardManager.h>
#include "BluetoothHIDManager.h"

namespace {
constexpr int SKIP_PAGE_MS = 700;
constexpr int goHomeMs = 500;

// 获取文件图标（文件夹或文件）
UIIcon getDataCapsuleIcon(const DataCapsuleEntry& entry) {
    if (entry.type == DataCapsuleEntry::FOLDER) {
        return UIIcon::Folder;
    }
    // 根据文件扩展名返回图标
    return UITheme::getFileIcon(entry.title);
}

// 格式化文件大小
std::string formatFileSize(uint32_t bytes) {
    if (bytes == 0) return "";
    char buf[16];
    if (bytes >= 1024u * 1024u) {
        snprintf(buf, sizeof(buf), "%.1fMB", bytes / (1024.0f * 1024.0f));
    } else if (bytes >= 1024u) {
        snprintf(buf, sizeof(buf), "%uKB", static_cast<unsigned>(bytes / 1024u));
    } else {
        snprintf(buf, sizeof(buf), "%uB", static_cast<unsigned>(bytes));
    }
    return buf;
}

// 判断是否为图片文件（包含带多余后缀的）
bool isImageFile(const std::string& filename) {
    std::string lower = filename;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    // 查找是否包含.png、.jpg、.jpeg、.bmp
    return lower.find(".png") != std::string::npos ||
           lower.find(".jpg") != std::string::npos ||
           lower.find(".jpeg") != std::string::npos ||
           lower.find(".bmp") != std::string::npos;
}

// 判断是否为标准图片扩展名（没有多余后缀）
bool isStandardImageExtension(const std::string& filename) {
    std::string lower = filename;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    // 检查是否以标准扩展名结尾
    if (lower.length() >= 4) {
        std::string last4 = lower.substr(lower.length() - 4);
        if (last4 == ".png" || last4 == ".jpg" || last4 == ".bmp") return true;
    }
    if (lower.length() >= 5) {
        std::string last5 = lower.substr(lower.length() - 5);
        if (last5 == ".jpeg") return true;
    }
    
    return false;
}

// 获取标准图片扩展名（去除多余后缀）
std::string getStandardImageExtension(const std::string& filename) {
    std::string lower = filename;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    // 查找.png、.jpg、.jpeg、.bmp的位置
    size_t pngPos = lower.find(".png");
    if (pngPos != std::string::npos) {
        return ".png";
    }
    
    size_t jpgPos = lower.find(".jpg");
    if (jpgPos != std::string::npos) {
        return ".jpg";
    }
    
    size_t jpegPos = lower.find(".jpeg");
    if (jpegPos != std::string::npos) {
        return ".jpeg";
    }
    
    size_t bmpPos = lower.find(".bmp");
    if (bmpPos != std::string::npos) {
        return ".bmp";
    }
    
    return "";
}
}

// 默认数据胶囊配置（当设置中未配置时使用）
constexpr const char* DC_DEFAULT_URL = "https://data.cstcloud.cn/dav";
constexpr const char* DC_USER_AGENT = "Zotero/8.0";

// 获取有效的WebDAV URL（优先使用设置值，否则使用默认值）
String getEffectiveWebdavUrl() {
    const char* url = SETTINGS.dcWebdavUrl;
    return (url && strlen(url) > 0) ? String(url) : String(DC_DEFAULT_URL);
}

// URL解析结构体
struct WebDavUrlParts {
    String host;
    int port;
    String basePath;  // e.g. "/dav"
    bool isHttps;
};

// 解析WebDAV URL，提取host/port/basePath
WebDavUrlParts parseWebdavUrl(const String& url) {
    WebDavUrlParts parts;
    parts.port = 443;
    parts.isHttps = true;
    parts.basePath = "/";

    int start = 0;
    if (url.startsWith("https://")) {
        parts.isHttps = true;
        parts.port = 443;
        start = 8;
    } else if (url.startsWith("http://")) {
        parts.isHttps = false;
        parts.port = 80;
        start = 7;
    }

    int pathStart = url.indexOf('/', start);
    if (pathStart == -1) {
        parts.host = url.substring(start);
        parts.basePath = "/";
    } else {
        parts.host = url.substring(start, pathStart);
        parts.basePath = url.substring(pathStart);
    }

    // Check for port in host (e.g. "example.com:8443")
    int colonPos = parts.host.indexOf(':');
    if (colonPos != -1) {
        parts.port = parts.host.substring(colonPos + 1).toInt();
        parts.host = parts.host.substring(0, colonPos);
    }

    // Remove trailing slash from basePath
    while (parts.basePath.length() > 1 && parts.basePath.endsWith("/")) {
        parts.basePath = parts.basePath.substring(0, parts.basePath.length() - 1);
    }

    return parts;
}

// URL编码函数（保留/字符用于路径分隔）
std::string urlEncodePath(const std::string& str) {
    std::string encoded;
    for (unsigned char c : str) {
        // 保留字母、数字和路径分隔符
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            encoded += c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            encoded += buf;
        }
    }
    return encoded;
}

// 静态成员初始化
std::vector<DataCapsuleEntry> DataCapsuleBrowserActivity::entries;
std::string DataCapsuleBrowserActivity::currentPath = "";
std::vector<std::string> DataCapsuleBrowserActivity::navigationHistory;
int DataCapsuleBrowserActivity::selectorIndex = 0;
std::string DataCapsuleBrowserActivity::errorMessage = "";
std::string DataCapsuleBrowserActivity::statusMessage = "";
DataCapsuleBrowserActivity::BrowserState DataCapsuleBrowserActivity::state = DataCapsuleBrowserActivity::BrowserState::CHECK_WIFI;
SemaphoreHandle_t DataCapsuleBrowserActivity::renderingMutex = nullptr;
TaskHandle_t DataCapsuleBrowserActivity::displayTaskHandle = nullptr;
bool DataCapsuleBrowserActivity::updateRequired = false;

// ===================== 内嵌WebDAV客户端 =====================
class DataCapsuleWebDAV {
public:
    void begin(const char* user, const char* pass, const char* webdavUrl) {
        _user = user;
        _pass = pass;
        auto parts = parseWebdavUrl(String(webdavUrl));
        _host = parts.host;
        _port = parts.port;
        _basePath = parts.basePath;
        _isHttps = parts.isHttps;
    }

    const String& getBasePath() const { return _basePath; }

    int propfind(const char* relativePath, String& result) {
        std::unique_ptr<WiFiClient> clientPtr;

        if (_isHttps) {
            auto* secureClient = new WiFiClientSecure();
            secureClient->setInsecure();
            clientPtr.reset(secureClient);
        } else {
            clientPtr.reset(new WiFiClient());
        }

        if (!clientPtr->connect(_host.c_str(), _port)) {
            Serial.println("[DC] Failed to connect to server");
            return 0;
        }

        // 构建请求路径（对relativePath进行URL编码以支持中文等非ASCII字符）
        String requestPath = _basePath;
        if (relativePath && strlen(relativePath) > 0) {
            // 对相对路径进行URL编码
            std::string encodedPath = urlEncodePath(relativePath);
            if (encodedPath[0] != '/') requestPath += "/";
            requestPath += encodedPath.c_str();
        }
        if (!requestPath.endsWith("/")) requestPath += "/";

        Serial.printf("[DC] PROPFIND path: %s\n", requestPath.c_str());

        // Base64 认证
        String authStr = _user + ":" + _pass;
        String base64Auth = base64::encode(authStr);
        base64Auth.trim();

        // 发送 PROPFIND 请求（关键：使用 Zotero User-Agent）
        clientPtr->print("PROPFIND ");
        clientPtr->print(requestPath);
        clientPtr->println(" HTTP/1.1");
        clientPtr->println("Host: " + _host);
        clientPtr->println("User-Agent: " + String(DC_USER_AGENT));
        clientPtr->println("Authorization: Basic " + base64Auth);
        clientPtr->println("Depth: 1");
        clientPtr->println("Content-Length: 0");
        clientPtr->println("Connection: close");
        clientPtr->println();

        // 读取响应：逐行读取 HTTP 响应头，提取状态码、Transfer-Encoding 和 Content-Length
        unsigned long timeout = millis() + 8000;
        auto readLineWithTimeout = [&](WiFiClient* c) -> String {
            String line = "";
            unsigned long t = millis() + 3000;
            while (millis() < t) {
                if (c->available()) {
                    char ch = c->read();
                    if (ch == '\n') return line;
                    if (ch != '\r') line += ch;
                } else {
                    delay(1);
                }
            }
            return line;
        };

        int httpCode = 0;
        bool isChunked = false;
        int contentLength = -1;

        // 读取状态行
        String statusLine = readLineWithTimeout(clientPtr.get());
        if (statusLine.startsWith("HTTP/1.1 ") || statusLine.startsWith("HTTP/1.0 ")) {
            httpCode = statusLine.substring(9, 12).toInt();
        } else {
            Serial.println("[DC] Invalid response: " + statusLine);
            clientPtr->stop();
            return 0;
        }

        // 读取并跳过所有 HTTP 响应头
        while (millis() < timeout) {
            String hdrLine = readLineWithTimeout(clientPtr.get());
            if (hdrLine.length() == 0) break;  // 空行 = 头部结束
            String lower = hdrLine;
            lower.toLowerCase();
            if (lower.startsWith("transfer-encoding:") && lower.indexOf("chunked") != -1) {
                isChunked = true;
            } else if (lower.startsWith("content-length:")) {
                contentLength = lower.substring(15).toInt();
            }
        }

        result = "";
        if (httpCode == 207) {
            // 预分配足够的连续内存，避免反复扩容时因堆碎片失败
            if (contentLength > 0) {
                if (!result.reserve(contentLength + 1)) {
                    Serial.printf("[DC] reserve(%d) failed, freeHeap=%u\n", contentLength + 1, ESP.getFreeHeap());
                }
            }
            if (isChunked) {
                // 解码 chunked transfer encoding
                timeout = millis() + 8000;
                while (millis() < timeout) {
                    String chunkSizeLine = readLineWithTimeout(clientPtr.get());
                    chunkSizeLine.trim();
                    if (chunkSizeLine.length() == 0) continue;
                    int chunkSize = (int)strtol(chunkSizeLine.c_str(), nullptr, 16);
                    if (chunkSize == 0) break;  // 最终 chunk
                    int remaining = chunkSize;
                    unsigned long chunkTimeout = millis() + 3000;
                    while (remaining > 0 && millis() < chunkTimeout) {
                        if (clientPtr->available()) {
                            char buf[128];
                            int toRead = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
                            int got = clientPtr->read((uint8_t*)buf, toRead);
                            if (got > 0) {
                                result.concat(buf, got);
                                remaining -= got;
                                chunkTimeout = millis() + 3000;
                            }
                        } else {
                            delay(1);
                        }
                    }
                    // 读取 chunk 末尾的 \r\n
                    readLineWithTimeout(clientPtr.get());
                }
            } else {
                // 非 chunked：按 Content-Length 精确读取，每收到数据重置超时
                timeout = millis() + 10000;
                int totalRead = 0;
                while (millis() < timeout) {
                    if (clientPtr->available()) {
                        char buf[256];
                        int toRead = (int)sizeof(buf);
                        if (contentLength > 0) {
                            int rem = contentLength - totalRead;
                            if (rem <= 0) break;
                            if (toRead > rem) toRead = rem;
                        }
                        int got = clientPtr->read((uint8_t*)buf, toRead);
                        if (got > 0) {
                            result.concat(buf, got);
                            totalRead += got;
                            timeout = millis() + 5000;  // 有数据就重置超时
                            if (contentLength > 0 && totalRead >= contentLength) break;
                        }
                    } else if (!clientPtr->connected()) {
                        break;
                    } else {
                        delay(1);
                    }
                }
                Serial.printf("[DC] non-chunked read: got %d / %d bytes\n", totalRead, contentLength);
            }
        } else {
            timeout = millis() + 3000;
            while ((clientPtr->connected() || clientPtr->available()) && millis() < timeout) {
                if (clientPtr->available()) result += clientPtr->readString();
                else delay(5);
            }
            Serial.println("[DC] Error response: " + result);
        }

        clientPtr->stop();
        return httpCode;
    }

private:
    String _user;
    String _pass;
    String _host;
    int _port = 443;
    String _basePath = "/dav";
    bool _isHttps = true;
};

// ===================== Activity 实现 =====================
void DataCapsuleBrowserActivity::taskTrampoline(void* param) {
    auto* self = static_cast<DataCapsuleBrowserActivity*>(param);
    self->displayTaskLoop();
}

void DataCapsuleBrowserActivity::displayTaskLoop() {
    while (true) {
        if (updateRequired) {
            updateRequired = false;
            xSemaphoreTake(renderingMutex, portMAX_DELAY);
            render();
            xSemaphoreGive(renderingMutex);
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void DataCapsuleBrowserActivity::onEnter() {
    ActivityWithSubactivity::onEnter();

    renderingMutex = xSemaphoreCreateMutex();
    state = BrowserState::CHECK_WIFI;
    entries.clear();
    navigationHistory.clear();
    currentPath = "";
    selectorIndex = 0;
    errorMessage.clear();
    statusMessage = L(Str::kCheckWifi);
    updateRequired = true;

    xTaskCreate(&DataCapsuleBrowserActivity::taskTrampoline, "DataCapsuleBrowserTask",
                4096, this, 1, &displayTaskHandle);

    autoConnectAttempted = false;
    autoConnectStartTime = 0;
    justEntered = true;  // 标记刚进入，等待按钮释放
    checkAndConnectWifi();
}

void DataCapsuleBrowserActivity::onExit() {
    // 重置自动连接状态
    autoConnectAttempted = false;

    // 关闭WiFi
    WiFi.mode(WIFI_OFF);

    if (displayTaskHandle) {
        vTaskDelete(displayTaskHandle);
        displayTaskHandle = nullptr;
    }
    if (renderingMutex) {
        vSemaphoreDelete(renderingMutex);
        renderingMutex = nullptr;
    }
    ActivityWithSubactivity::onExit();
}

void DataCapsuleBrowserActivity::loop() {
    // 处理WiFi选择子页面
    if (state == BrowserState::WIFI_SELECTION) {
        ActivityWithSubactivity::loop();
        return;
    }

    auto touchBack = [this]() { return mappedInput.hasTouch() && mappedInput.wasBackGesture(); };

    // 错误状态：重试/返回
    if (state == BrowserState::ERROR) {
        if (touchBack()) {
            if (entries.empty() && navigationHistory.empty()) onGoHome();
            else if (entries.empty()) {
                state = BrowserState::LOADING;
                statusMessage = L(Str::kLoading);
                updateRequired = true;
                fetchFeed(currentPath);
            } else {
                state = BrowserState::BROWSING;
                updateRequired = true;
            }
            return;
        }
        if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= goHomeMs) {
            if (WiFi.status() == WL_CONNECTED) {
                state = BrowserState::LOADING;
                statusMessage = L(Str::kLoading);
                updateRequired = true;
                fetchFeed(currentPath);
            } else {
                launchWifiSelection();
            }
        } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
            // 短按返回：如果从未成功加载过（如认证失败），则退出到主页；否则返回文件列表
            if (entries.empty() && navigationHistory.empty()) {
                // 从未成功加载过，退出到主页
                onGoHome();
            } else if (entries.empty()) {
                // 曾经加载过但现在为空，重新加载
                state = BrowserState::LOADING;
                statusMessage = L(Str::kLoading);
                updateRequired = true;
                fetchFeed(currentPath);
            } else {
                // 有文件列表，返回浏览状态
                state = BrowserState::BROWSING;
                updateRequired = true;
            }
        }
        return;
    }

    // 检查WiFi/加载中
    if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
        if (state == BrowserState::CHECK_WIFI && autoConnectAttempted) {
            checkAutoConnectProgress();
        }
        if (mappedInput.wasReleased(MappedInputManager::Button::Back) || touchBack()) {
            if (autoConnectAttempted) {
                autoConnectAttempted = false;
                WiFi.disconnect();
            }
            exitActivity();
        }
        return;
    }

    // 浏览目录状态
    if (state == BrowserState::BROWSING) {
        auto handleBrowseTouch = [this]() -> bool {
            if (!mappedInput.hasTouch()) return false;
            auto metrics = UITheme::getInstance().getMetrics();
            const int pageHeight = renderer.getScreenHeight();
            const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
            const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
            const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, false);
            const int itemCount = static_cast<int>(entries.size());
            M4ListTouchPolicy::Event te{};
            te.backGesture = mappedInput.wasBackGesture();
            const auto sw = mappedInput.wasSwipe();
            if (sw == MappedInputManager::SwipeDir::Up) te.swipe = M4ListTouchPolicy::Swipe::Up;
            else if (sw == MappedInputManager::SwipeDir::Down) te.swipe = M4ListTouchPolicy::Swipe::Down;
            else if (sw == MappedInputManager::SwipeDir::Left) te.swipe = M4ListTouchPolicy::Swipe::Left;
            else if (sw == MappedInputManager::SwipeDir::Right) te.swipe = M4ListTouchPolicy::Swipe::Right;
            int dx = 0, dy = 0, tx = 0, ty = 0;
            te = M4ListTouchPolicy::mergeFrame(te.backGesture, te.swipe, mappedInput.wasScreenTouchDown(dx, dy), dx, dy,
                                               mappedInput.wasScreenTapped(tx, ty), tx, ty);
            M4ListTouchPolicy::ListLayout layout;
            layout.listTop = contentTop;
            layout.listHeight = contentHeight;
            layout.rowStep = metrics.listRowHeight;
            layout.itemCount = itemCount;
            layout.selectedIndex = selectorIndex;
            int hit = -1;
            const auto act = M4ListTouchPolicy::resolveList(te, layout, hit);
            if (act == M4ListTouchPolicy::Action::Back) {
                if (navigationHistory.empty()) onGoHome();
                else navigateBack();
                return true;
            }
            if (act == M4ListTouchPolicy::Action::PageDown || act == M4ListTouchPolicy::Action::PageUp) {
                if (itemCount > 0) {
                    selectorIndex = M4ListTouchPolicy::applyPage(selectorIndex, itemCount, pageItems,
                                                                 act == M4ListTouchPolicy::Action::PageDown);
                    updateRequired = true;
                }
                return true;
            }
            if (act == M4ListTouchPolicy::Action::Select && hit >= 0) {
                if (selectorIndex != hit) {
                    selectorIndex = hit;
                    updateRequired = true;
                }
                return true;
            }
            if (act == M4ListTouchPolicy::Action::Activate && hit >= 0) {
                selectorIndex = hit;
                const auto& entry = entries[selectorIndex];
                if (entry.type == DataCapsuleEntry::FOLDER) navigateToEntry(entry);
                else downloadBook(entry);
                return true;
            }
            return te.backGesture || te.swipe != M4ListTouchPolicy::Swipe::None || te.touchDown || te.tap;
        };

        // 如果刚进入Activity，等待确认按钮完全释放后再响应
        if (justEntered) {
            if (handleBrowseTouch()) return;
            if (!mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
                !mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
                justEntered = false;  // 按钮已完全释放，开始正常响应
            }
            // 继续处理其他按钮（上/下/返回）
            const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up);
            const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down);
            const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;
            const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, false);

            if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
                state = BrowserState::LOADING;
                statusMessage = L(Str::kRefreshing);
                updateRequired = true;
                fetchFeed(currentPath);
            } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
                if (navigationHistory.empty()) {
                    onGoHome();
                } else {
                    navigateBack();
                }
            } else if (prevReleased && !entries.empty()) {
                if (skipPage) {
                    selectorIndex = ((selectorIndex / pageItems - 1) * pageItems + entries.size()) % entries.size();
                } else {
                    selectorIndex = (selectorIndex + entries.size() - 1) % entries.size();
                }
                updateRequired = true;
            } else if (nextReleased && !entries.empty()) {
                if (skipPage) {
                    selectorIndex = ((selectorIndex / pageItems + 1) * pageItems) % entries.size();
                } else {
                    selectorIndex = (selectorIndex + 1) % entries.size();
                }
                updateRequired = true;
            } else if (mappedInput.wasReleased(MappedInputManager::Button::Right) && !entries.empty()) {
                // Right按钮：往后翻一整页
                selectorIndex = std::min(selectorIndex + pageItems, static_cast<int>(entries.size()) - 1);
                updateRequired = true;
            }
            return;
        }

        if (handleBrowseTouch()) return;

        const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up);
        const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down);
        const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;
        const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, false);

        if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
            if (!entries.empty()) {
                const auto& entry = entries[selectorIndex];
                if (entry.type == DataCapsuleEntry::FOLDER) {
                    navigateToEntry(entry);
                } else {
                    downloadBook(entry);
                }
            }
        } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
            state = BrowserState::LOADING;
            statusMessage = L(Str::kRefreshing);
            updateRequired = true;
            fetchFeed(currentPath);
        } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
            if (navigationHistory.empty()) {
                onGoHome();
            } else {
                navigateBack();
            }
        } else if (prevReleased && !entries.empty()) {
            if (skipPage) {
                selectorIndex = ((selectorIndex / pageItems - 1) * pageItems + entries.size()) % entries.size();
            } else {
                selectorIndex = (selectorIndex + entries.size() - 1) % entries.size();
            }
            updateRequired = true;
        } else if (nextReleased && !entries.empty()) {
            if (skipPage) {
                selectorIndex = ((selectorIndex / pageItems + 1) * pageItems) % entries.size();
            } else {
                selectorIndex = (selectorIndex + 1) % entries.size();
            }
            updateRequired = true;
        } else if (mappedInput.wasReleased(MappedInputManager::Button::Right) && !entries.empty()) {
            // Right按钮：往后翻一整页
            selectorIndex = std::min(selectorIndex + pageItems, static_cast<int>(entries.size()) - 1);
            updateRequired = true;
        }
    }
}

bool DataCapsuleBrowserActivity::endsWith(const std::string& str, const std::string& suffix) {
    if (suffix.size() > str.size()) return false;
    return std::equal(suffix.rbegin(), suffix.rend(), str.rbegin(),
                      [](char a, char b) { return tolower(a) == tolower(b); });
}

void DataCapsuleBrowserActivity::drawLocalButtonHints(const char* btn1, const char* btn2, const char* btn3, const char* btn4) const {
    const int pageHeight = renderer.getScreenHeight();
    constexpr int buttonWidth = 80;
    constexpr int smallButtonHeight = 15;
    constexpr int buttonHeight = 40;
    constexpr int buttonY = 40;
    constexpr int textYOffset = 7;
    constexpr int cornerRadius = 6;
    constexpr int buttonPositions[] = {58, 146, 254, 342};
    const char* labels[] = {btn1, btn2, btn3, btn4};

    for (int i = 0; i < 4; i++) {
        const int x = buttonPositions[i];
        renderer.fillRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, false);
        if (labels[i] != nullptr && labels[i][0] != '\0') {
            renderer.drawRoundedRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, 1, cornerRadius, true, true, false, false, true);
            const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
            const int textX = x + (buttonWidth - 1 - textWidth) / 2;
            renderer.drawText(SMALL_FONT_ID, textX, pageHeight - buttonY + textYOffset, labels[i]);
        } else {
            renderer.drawRoundedRect(x, pageHeight - smallButtonHeight, buttonWidth, smallButtonHeight, 1, cornerRadius, true, true, false, false, true);
        }
    }
}

void DataCapsuleBrowserActivity::render() const {
    renderer.clearScreen();

    const auto pageWidth = renderer.getScreenWidth();
    const auto pageHeight = renderer.getScreenHeight();
    auto metrics = UITheme::getInstance().getMetrics();

    // 标题栏：显示当前路径
    std::string pathDisplay = currentPath.empty() ? L(Str::kDataCapsule) : currentPath;
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, pathDisplay.c_str());

    // 检查WiFi状态
    if (state == BrowserState::CHECK_WIFI) {
        M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight / 2, statusMessage.c_str());
        const auto labels = mappedInput.mapLabels(L(Str::kBackShort), "", "", "");
        GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
        renderer.displayBuffer();
        return;
    }

    // 加载中状态
    if (state == BrowserState::LOADING) {
        M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight / 2, statusMessage.c_str());
        const auto labels2 = mappedInput.mapLabels(L(Str::kBackShort), "", "", "");
        GUI.drawButtonHints(renderer, labels2.btn1, labels2.btn2, labels2.btn3, labels2.btn4);
        renderer.displayBuffer();
        return;
    }

    // 下载中
    if (state == BrowserState::DOWNLOADING) {
        M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight / 2 - 40, L(Str::kDownloading));
        M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight / 2 - 10, statusMessage.c_str());
        if (downloadTotal > 0) {
            const int barWidth = renderer.getScreenWidth() - 100;
            constexpr int barHeight = 20;
            constexpr int barX = 50;
            const int barY = renderer.getScreenHeight() / 2 + 20;
            GUI.drawProgressBar(renderer, Rect{barX, barY, barWidth, barHeight}, downloadProgress, downloadTotal);
        }
        renderer.displayBuffer();
        return;
    }

    // 错误状态
    if (state == BrowserState::ERROR) {
        M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight / 2 - 20, L(Str::kFailed));
        M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight / 2 + 10, errorMessage.c_str());
        // 错误状态下强制显示按钮提示
        const auto labels = mappedInput.mapLabels(L(Str::kBack), L(Str::kLongPressRetry), "", "");
        GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
        renderer.displayBuffer();
        return;
    }

    // 浏览目录状态
    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

    if (entries.empty()) {
        // 列表为空时，不显示"无文件"，保持空白（因为可能是首次加载）
        // 只有在明确加载完成后才显示空提示
    } else {
        // 使用与文件管理相同的GUI.drawList绘制列表
        GUI.drawList(
            renderer, Rect{0, contentTop, pageWidth, contentHeight}, entries.size(), selectorIndex,
            [this](int index) -> std::string {
                // 直接返回名称，不添加>前缀，drawList内部会自动截断
                return entries[index].title;
            },
            nullptr,  // 无副标题
            [this](int index) -> UIIcon {
                // 返回图标
                return getDataCapsuleIcon(entries[index]);
            },
            [this](int index) -> std::string {
                // 返回文件大小
                return formatFileSize(entries[index].fileSize);
            });
    }

    // 按钮提示（始终显示，force=true强制显示不受系统设置影响）
    // 根据选中的是目录还是文件动态显示按钮文字
    const char* confirmLabel = L(Str::kOpen);
    if (!entries.empty() && selectorIndex >= 0 && selectorIndex < static_cast<int>(entries.size())) {
        if (entries[selectorIndex].type == DataCapsuleEntry::FOLDER) {
            confirmLabel = L(Str::kOpen);
        } else {
            confirmLabel = L(Str::kDownload);
        }
    }
    const auto labels = mappedInput.mapLabels(L(Str::kBack), confirmLabel, L(Str::kRefresh), L(Str::kNextPage));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);

    renderer.displayBuffer();
}

void DataCapsuleBrowserActivity::fetchFeed(const std::string& subPath) {
    std::string username = SETTINGS.dcUsername;
    std::string password = SETTINGS.dcPassword;

    if (username.empty() || password.empty()) {
        state = BrowserState::ERROR;
        errorMessage = L(Str::kPleaseConfigUserPass);
        updateRequired = true;
        return;
    }

    entries.clear();

    String effectiveUrl = getEffectiveWebdavUrl();
    DataCapsuleWebDAV dav;
    dav.begin(username.c_str(), password.c_str(), effectiveUrl.c_str());

    Serial.printf("\n[DC] Fetching: %s (URL: %s)\n", subPath.c_str(), effectiveUrl.c_str());
    String xmlResult;
    int responseCode = dav.propfind(subPath.c_str(), xmlResult);
    
    if (responseCode == 207) {
        parseXmlEntries(xmlResult, subPath, dav.getBasePath(), entries);
        Serial.printf("[DC] Found %d entries\n", entries.size());
    } else {
        Serial.printf("[DC] Failed with code: %d\n", responseCode);
        state = BrowserState::ERROR;
        
        // 根据HTTP状态码提供准确的错误提示
        if (responseCode == 401) {
            errorMessage = L(Str::kUserPassError);
        } else if (responseCode == 403) {
            errorMessage = L(Str::kAccessDenied);
        } else if (responseCode == 404) {
            errorMessage = L(Str::kWebdavPathNotExist);
        } else if (responseCode == 0) {
            errorMessage = L(Str::kNetworkConnectFailed);
        } else {
            char buf[32];
            snprintf(buf, sizeof(buf), "HTTP %d", responseCode);
            errorMessage = buf;
        }
        updateRequired = true;
        return;
    }

    selectorIndex = 0;
    state = BrowserState::BROWSING;
    updateRequired = true;
}

void DataCapsuleBrowserActivity::parseXmlEntries(const String& xmlResult, 
                                                  const std::string& basePath,
                                                  const String& urlBasePath,
                                                  std::vector<DataCapsuleEntry>& outEntries) {
    int xmlPos = 0;
    while (true) {
        // 查找 <d:response> 块
        int respStart = xmlResult.indexOf("<d:response>", xmlPos);
        if (respStart == -1) {
            respStart = xmlResult.indexOf("<D:response>", xmlPos);
        }
        if (respStart == -1) break;
        respStart += 12;  // 跳过 <d:response>
        
        int respEnd = xmlResult.indexOf("</d:response>", respStart);
        if (respEnd == -1) {
            respEnd = xmlResult.indexOf("</D:response>", respStart);
        }
        if (respEnd == -1) break;
        
        String responseBlock = xmlResult.substring(respStart, respEnd);
        xmlPos = respEnd + 13;
        
        // 从 response 块中提取 href（已编码的路径）
        int hrefStart = responseBlock.indexOf("<d:href>");
        if (hrefStart == -1) hrefStart = responseBlock.indexOf("<D:href>");
        if (hrefStart == -1) continue;
        hrefStart += 8;
        int hrefEnd = responseBlock.indexOf("</d:href>", hrefStart);
        if (hrefEnd == -1) hrefEnd = responseBlock.indexOf("</D:href>", hrefStart);
        if (hrefEnd == -1) continue;
        String href = responseBlock.substring(hrefStart, hrefEnd);
        
        // 从 response 块中提取 displayname（未编码的显示名）
        int nameStart = responseBlock.indexOf("<d:displayname>");
        if (nameStart == -1) nameStart = responseBlock.indexOf("<D:displayname>");
        if (nameStart == -1) continue;
        nameStart += 15;
        int nameEnd = responseBlock.indexOf("</d:displayname>", nameStart);
        if (nameEnd == -1) nameEnd = responseBlock.indexOf("</D:displayname>", nameStart);
        if (nameEnd == -1) continue;
        std::string fileName = responseBlock.substring(nameStart, nameEnd).c_str();

        if (fileName == "." || fileName == ".." || fileName.empty()) continue;

        // 过滤掉根目录本身（PROPFIND 返回的第一个条目是当前目录）
        if (basePath.empty() && fileName == "root") continue;

        // 过滤掉当前目录本身（通过比较href和当前路径）
        // 构建当前目录的完整href路径
        String currentDirHref = urlBasePath;
        if (!basePath.empty()) {
            // 对basePath进行URL编码以比较
            std::string encodedBasePath = urlEncodePath(basePath);
            currentDirHref += "/" + String(encodedBasePath.c_str());
        }
        // 去掉href末尾的斜杠再比较
        String hrefWithoutSlash = href;
        if (hrefWithoutSlash.endsWith("/")) {
            hrefWithoutSlash = hrefWithoutSlash.substring(0, hrefWithoutSlash.length() - 1);
        }
        String currentDirHrefWithoutSlash = currentDirHref;
        if (currentDirHrefWithoutSlash.endsWith("/")) {
            currentDirHrefWithoutSlash = currentDirHrefWithoutSlash.substring(0, currentDirHrefWithoutSlash.length() - 1);
        }
        if (hrefWithoutSlash == currentDirHrefWithoutSlash) {
            // 这是当前目录本身，跳过
            continue;
        }

        // 检查是否为文件夹
        bool isFolder = responseBlock.indexOf("<d:collection") != -1 || 
                        responseBlock.indexOf("<D:collection") != -1;

        // 提取文件大小（如果有）
        uint32_t fileSize = 0;
        if (!isFolder) {
            int sizeStart = responseBlock.indexOf("<d:getcontentlength>");
            if (sizeStart == -1) sizeStart = responseBlock.indexOf("<D:getcontentlength>");
            if (sizeStart != -1) {
                sizeStart += 20;  // 跳过 <d:getcontentlength>
                int sizeEnd = responseBlock.indexOf("</d:getcontentlength>", sizeStart);
                if (sizeEnd == -1) sizeEnd = responseBlock.indexOf("</D:getcontentlength>", sizeStart);
                if (sizeEnd != -1) {
                    String sizeStr = responseBlock.substring(sizeStart, sizeEnd);
                    fileSize = sizeStr.toInt();
                }
            }
        }

        DataCapsuleEntry entry;
        entry.title = fileName;
        entry.path = basePath.empty() ? fileName : basePath + "/" + fileName;
        entry.fileSize = fileSize;
        // 从 href 中提取编码后的相对路径（去掉URL basePath前缀）
        String urlPrefix = urlBasePath + "/";
        if (href.startsWith(urlPrefix)) {
            entry.encodedPath = href.substring(urlPrefix.length()).c_str();
        } else if (href.startsWith(urlBasePath) && href.length() > urlBasePath.length()) {
            entry.encodedPath = href.substring(urlBasePath.length()).c_str();
        } else {
            entry.encodedPath = href.c_str();
        }
        entry.type = isFolder ? DataCapsuleEntry::FOLDER : DataCapsuleEntry::BOOK_FILE;

        // 显示所有文件，不再过滤格式
        outEntries.push_back(entry);
    }
}

void DataCapsuleBrowserActivity::navigateToEntry(const DataCapsuleEntry& entry) {
    if (entry.type != DataCapsuleEntry::FOLDER) return;

    navigationHistory.push_back(currentPath);
    currentPath = entry.path;

    state = BrowserState::LOADING;
    statusMessage = L(Str::kLoading);
    entries.clear();
    selectorIndex = 0;
    updateRequired = true;

    fetchFeed(currentPath);
}

void DataCapsuleBrowserActivity::navigateBack() {
    if (navigationHistory.empty()) {
        exitActivity();
    } else {
        currentPath = navigationHistory.back();
        navigationHistory.pop_back();

        state = BrowserState::LOADING;
        statusMessage = L(Str::kLoading);
        entries.clear();
        selectorIndex = 0;
        updateRequired = true;

        fetchFeed(currentPath);
    }
}

void DataCapsuleBrowserActivity::checkAndConnectWifi() {
    if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        state = BrowserState::LOADING;
        statusMessage = L(Str::kLoading);
        updateRequired = true;
        fetchFeed(currentPath);
        return;
    }

    // 加载已保存的WiFi凭据
    if (renderingMutex) {
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        WIFI_STORE.loadFromFile();
        xSemaphoreGive(renderingMutex);
    } else {
        WIFI_STORE.loadFromFile();
    }
    const auto& credentials = WIFI_STORE.getCredentials();

    if (credentials.empty() || SETTINGS.wifiAlwaysReselect) {
        Serial.printf("[DC] No saved WiFi credentials or always reselect\n");
        launchWifiSelection();
        return;
    }

    const auto& cred = credentials[0];
    Serial.printf("[DC] Auto-connecting: %s\n", cred.ssid.c_str());

    // 禁用蓝牙
    try {
        auto& btMgr = BluetoothHIDManager::getInstance();
        if (btMgr.isEnabled()) {
            btMgr.disable();
        }
    } catch (...) {}

    WiFi.mode(WIFI_STA);
    WiFi.begin(cred.ssid.c_str(), cred.password.c_str());

    state = BrowserState::CHECK_WIFI;
    autoConnectAttempted = true;
    autoConnectStartTime = millis();
    statusMessage = std::string(L(Str::kConnectingTo)) + cred.ssid + "...";
    updateRequired = true;
}

void DataCapsuleBrowserActivity::checkAutoConnectProgress() {
    if (!autoConnectAttempted) return;

    wl_status_t status = WiFi.status();

    if (status == WL_CONNECTED) {
        Serial.printf("[DC] Connected: %s\n", WiFi.localIP().toString().c_str());
        autoConnectAttempted = false;
        state = BrowserState::LOADING;
        statusMessage = L(Str::kLoading);
        updateRequired = true;
        fetchFeed(currentPath);
        return;
    }

    if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
        Serial.printf("[DC] Connection failed: %d\n", status);
        autoConnectAttempted = false;
        WiFi.disconnect();
        launchWifiSelection();
        return;
    }

    if (millis() - autoConnectStartTime > 10000) {
        Serial.printf("[DC] Connection timeout\n");
        autoConnectAttempted = false;
        WiFi.disconnect();
        launchWifiSelection();
        return;
    }
}

void DataCapsuleBrowserActivity::launchWifiSelection() {
    state = BrowserState::WIFI_SELECTION;
    updateRequired = true;

    enterNewActivity(new WifiSelectionActivity(renderer, mappedInput,
                                               [this](const bool connected) { onWifiSelectionComplete(connected); }));
}

void DataCapsuleBrowserActivity::onWifiSelectionComplete(const bool connected) {
    exitActivity();

    if (connected) {
        Serial.printf("[DC] WiFi connected, loading...\n");
        state = BrowserState::LOADING;
        statusMessage = L(Str::kLoading);
        updateRequired = true;
        fetchFeed(currentPath);
    } else {
        Serial.printf("[DC] WiFi connection failed\n");
        WiFi.disconnect();
        WiFi.mode(WIFI_OFF);
        state = BrowserState::ERROR;
        errorMessage = L(Str::kWifiConnectFailed);
        updateRequired = true;
    }
}

void DataCapsuleBrowserActivity::downloadBook(const DataCapsuleEntry& book) {
    state = BrowserState::DOWNLOADING;
    statusMessage = book.title;
    downloadProgress = 0;
    downloadTotal = 0;
    updateRequired = true;

    // 构建下载URL（服务器返回的encodedPath已经是URL编码的）
    std::string effectiveUrl = getEffectiveWebdavUrl().c_str();
    std::string downloadUrl = effectiveUrl;
    // 确保URL和路径之间只有一个/
    if (!downloadUrl.empty() && downloadUrl.back() == '/') {
        downloadUrl.pop_back();
    }
    if (!book.encodedPath.empty() && book.encodedPath[0] != '/') {
        downloadUrl += "/";
    }
    downloadUrl += book.encodedPath;

    // 根据扩展名选择本地保存目录
    std::string targetDir;
    if (endsWith(book.title, ".pngtxt")) {
        targetDir = "/lock_screen/";
    } else if (endsWith(book.title, ".epdfont")) {
        targetDir = "/fonts";
    } else {
        targetDir = "/数据胶囊";
    }

    if (!targetDir.empty()) {
        SdMan.mkdir(targetDir.c_str());
    }

    std::string safeFilename = StringUtils::sanitizeFilename(book.title, 200);
    std::string localPath;
    if (targetDir.empty()) {
        localPath = "/" + safeFilename;
    } else {
        localPath = targetDir + "/" + safeFilename;
    }

    // 检查是否为图片文件
    bool isImage = isImageFile(book.title);
    std::string actualDownloadUrl = downloadUrl;
    std::string actualLocalPath = localPath;
    
    if (isImage) {
        // 检查是否为标准扩展名（没有多余后缀）
        if (isStandardImageExtension(book.title)) {
            // 标准扩展名，提示用户修改后缀
            state = BrowserState::ERROR;
            std::string ext = getStandardImageExtension(book.title);
            errorMessage = "请将该文件后缀改为" + ext + "2再下载";
            updateRequired = true;
            return;
        }
        
        // 带多余后缀的图片文件：下载原文件，保存时去除多余后缀
        std::string standardExt = getStandardImageExtension(book.title);
        
        // 找到标准扩展名的位置
        std::string lowerTitle = book.title;
        std::transform(lowerTitle.begin(), lowerTitle.end(), lowerTitle.begin(), ::tolower);
        size_t extPos = lowerTitle.find(standardExt);
        
        // 构建本地保存文件名：只保留到标准扩展名
        std::string localFilename = book.title.substr(0, extPos + standardExt.length());
        safeFilename = StringUtils::sanitizeFilename(localFilename, 200);
        if (targetDir.empty()) {
            actualLocalPath = "/" + safeFilename;
        } else {
            actualLocalPath = targetDir + "/" + safeFilename;
        }
        
        Serial.printf("[DC] Image file with suffix: downloading %s -> saving as %s\n", downloadUrl.c_str(), actualLocalPath.c_str());
    } else {
        Serial.printf("[DC] Downloading: %s -> %s\n", downloadUrl.c_str(), localPath.c_str());
    }

    // 使用数据胶囊专用下载函数
    const auto result = HttpDownloader::downloadToFile_dc(
        actualDownloadUrl,
        actualLocalPath,
        [this](size_t downloaded, size_t total) {
            downloadProgress = downloaded;
            downloadTotal = total;
            updateRequired = true;
        }
    );

    if (result == HttpDownloader::OK) {
        Serial.printf("[DC] Download complete\n");
        state = BrowserState::BROWSING;
        updateRequired = true;
    } else {
        state = BrowserState::ERROR;
        char codeStr[32];
        snprintf(codeStr, sizeof(codeStr), "下载失败: HTTP %d", (int)result);
        errorMessage = codeStr;
        Serial.printf("[DC] Download failed: %d\n", (int)result);
        updateRequired = true;
    }
}
