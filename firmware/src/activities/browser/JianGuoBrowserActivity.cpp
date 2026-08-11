#include "JianGuoBrowserActivity.h"

#include <GfxRenderer.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <base64.h>  // 显式包含Base64头文件

#include "CrossPointSettings.h"
#include "I18n.h"
#include "MappedInputManager.h"
#include "WifiCredentialStore.h"
#include "BluetoothHIDManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"

#include <HTTPClient.h>
#include "network/HttpDownloader.h"
#include "util/M4ListTouchPolicy.h"
#include "util/StringUtils.h"
#include "util/UrlUtils.h"
#include <Epub.h>

// 新增：SPIFFS存储依赖（仅初始化，不做文件写入）
// #include <SPIFFS.h>  // 已移除，文件操作全部走SD卡

#include <cctype>
#include <functional>
#include <iomanip>
#include <sstream>

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
constexpr unsigned long goHomeMs = 500;
}

static std::string urlEncode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::setw(2) << static_cast<int>(c);
        }
    }
    return escaped.str();
}

// ===================== 内嵌ESPWebDAV核心代码（修正版）=====================
class ESPWebDAV {
public:
    void begin(const char* url, const char* user, const char* pass) {
        _baseUrl = url;
        _user = user;
        _pass = pass;
        _timeout = 5000;
        _basePath = "";  // 新增：初始化
    }

    void setTimeout(int timeout) {
        _timeout = timeout;
    }

    // 新增：设置自定义基础路径
    void setBasePath(const char* basePath) {
        _basePath = basePath;
    }

    // 新增：删除文件
    int deleteFile(const char* relativePath) {
        WiFiClientSecure client;
        client.setInsecure();

        const char* host = "dav.jianguoyun.com";
        const int port = 443;

        if (!client.connect(host, port)) {
            Serial.println("[DAV] Failed to connect to server");
            return 0;
        }

        // 构建请求路径
        String requestPath = String("/dav/");
        if (_basePath == "__FULL_PATH__") {
            // 直接使用 relativePath 作为完整路径
            if (relativePath && strlen(relativePath) > 0) {
                requestPath += String(relativePath);
            }
        } else if (_basePath.length() > 0) {
            requestPath += _basePath;
            if (relativePath && strlen(relativePath) > 0) {
                if (requestPath[requestPath.length()-1] != '/') requestPath += "/";
                requestPath += relativePath;
            }
        } else {
            requestPath += String(SETTINGS.jgBookFolder);
            if (relativePath && strlen(relativePath) > 0) {
                if (requestPath[requestPath.length()-1] != '/') requestPath += "/";
                requestPath += relativePath;
            }
        }

        Serial.printf("[DAV] DELETE 请求路径：%s\n", requestPath.c_str());

        // Base64 认证
        String authStr = _user + ":" + _pass;
        String base64Auth = base64::encode(authStr);
        base64Auth.trim();

        // 发送 DELETE 请求
        client.print("DELETE ");
        client.print(requestPath);
        client.println(" HTTP/1.1");
        client.println("Host: dav.jianguoyun.com");
        client.println("Authorization: Basic " + base64Auth);
        client.println("Connection: close");
        client.println();

        // 读取响应
        unsigned long timeout = millis() + 8000;
        String responseLine = "";
        while (millis() < timeout && client.connected()) {
            if (client.available()) {
                responseLine = client.readStringUntil('\n');
                break;
            }
            delay(1);
        }

        int httpCode = 0;
        if (responseLine.startsWith("HTTP/1.1 ")) {
            httpCode = responseLine.substring(9, 12).toInt();
            Serial.printf("[DAV] DELETE 响应码：%d\n", httpCode);
        } else {
            Serial.println("[DAV] Invalid response: " + responseLine);
        }

        client.stop();
        return httpCode;  // 204 = No Content (成功删除)
    }

    int propfind(const char* relativePath, String& result) {
        WiFiClientSecure client;
        client.setInsecure();

        const char* host = "dav.jianguoyun.com";
        const int port = 443;

        if (!client.connect(host, port)) {
            Serial.println("[DAV] Failed to connect to server");
            return 0;
        }

        // === 构建请求路径（分段URL编码以支持中文等非ASCII字符） ===
        String requestPath = "/dav/";
        
        if (relativePath && strlen(relativePath) > 0) {
            // 分段编码：只对每个路径段编码，不编码/
            String pathStr = String(relativePath);
            int startPos = 0;
            bool first = true;
            
            while (true) {
                int slashPos = pathStr.indexOf('/', startPos);
                String segment;
                
                if (slashPos == -1) {
                    // 最后一段
                    segment = pathStr.substring(startPos);
                } else {
                    // 中间段
                    segment = pathStr.substring(startPos, slashPos);
                }
                
                // 对当前段进行URL编码
                if (segment.length() > 0) {
                    if (!first) requestPath += "/";
                    requestPath += String(urlEncode(segment.c_str()).c_str());
                    first = false;
                }
                
                if (slashPos == -1) break;
                startPos = slashPos + 1;
            }
        }
        if (!requestPath.endsWith("/")) requestPath += "/";

        // 【调试】打印路径
        Serial.printf("[DAV] PROPFIND 请求路径: %s\n", requestPath.c_str());

        // Base64 认证
        String authStr = _user + ":" + _pass;
        String base64Auth = base64::encode(authStr);
        base64Auth.trim();

        client.print("PROPFIND ");
        client.print(requestPath);
        client.println(" HTTP/1.1");
        client.println("Host: dav.jianguoyun.com");
        client.println("Authorization: Basic " + base64Auth);
        client.println("Depth: 1");
        client.println("Content-Length: 0");
        client.println("Connection: close");
        client.println();

        // 读取响应：逐行读取 HTTP 响应头，提取状态码、Transfer-Encoding 和 Content-Length
        unsigned long timeout = millis() + 8000;
        auto readLineWithTimeout = [&](WiFiClientSecure& c) -> String {
            String line = "";
            unsigned long t = millis() + 3000;
            while (millis() < t) {
                if (c.available()) {
                    char ch = c.read();
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
        String statusLine = readLineWithTimeout(client);
        if (statusLine.startsWith("HTTP/1.1 ") || statusLine.startsWith("HTTP/1.0 ")) {
            httpCode = statusLine.substring(9, 12).toInt();
        } else {
            Serial.println("[DAV] Invalid response: " + statusLine);
            client.stop();
            return 0;
        }

        // 读取并跳过所有 HTTP 响应头
        while (millis() < timeout) {
            String hdrLine = readLineWithTimeout(client);
            if (hdrLine.length() == 0) break;  // 空行 = 头部结束
            String lower = hdrLine;
            lower.toLowerCase();
            if (lower.startsWith("transfer-encoding:") && lower.indexOf("chunked") != -1) {
                isChunked = true;
            } else if (lower.startsWith("content-length:")) {
                contentLength = lower.substring(15).toInt();
            }
        }
        Serial.printf("[DAV] isChunked=%d contentLength=%d\n", (int)isChunked, contentLength);

        result = "";
        if (httpCode == 207) {
            // 预分配足够的连续内存，避免反复扩容时因堆碎片失败
            if (contentLength > 0) {
                if (!result.reserve(contentLength + 1)) {
                    Serial.printf("[DAV] reserve(%d) failed, freeHeap=%u\n", contentLength + 1, ESP.getFreeHeap());
                }
            }
            if (isChunked) {
                // 解码 chunked transfer encoding
                timeout = millis() + 8000;
                while (millis() < timeout) {
                    // 读取 chunk 大小行（十六进制）
                    String chunkSizeLine = readLineWithTimeout(client);
                    chunkSizeLine.trim();
                    if (chunkSizeLine.length() == 0) continue;
                    int chunkSize = (int)strtol(chunkSizeLine.c_str(), nullptr, 16);
                    if (chunkSize == 0) break;  // 最终 chunk
                    // 读取 chunkSize 字节
                    int remaining = chunkSize;
                    unsigned long chunkTimeout = millis() + 3000;
                    while (remaining > 0 && millis() < chunkTimeout) {
                        if (client.available()) {
                            char buf[128];
                            int toRead = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
                            int got = client.read((uint8_t*)buf, toRead);
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
                    readLineWithTimeout(client);
                }
            } else {
                // 非 chunked：按 Content-Length 精确读取，每收到数据重置超时
                timeout = millis() + 10000;
                int totalRead = 0;
                while (millis() < timeout) {
                    if (client.available()) {
                        char buf[256];
                        int toRead = (int)sizeof(buf);
                        if (contentLength > 0) {
                            int rem = contentLength - totalRead;
                            if (rem <= 0) break;
                            if (toRead > rem) toRead = rem;
                        }
                        int got = client.read((uint8_t*)buf, toRead);
                        if (got > 0) {
                            result.concat(buf, got);
                            totalRead += got;
                            timeout = millis() + 5000;  // 有数据就重置超时
                            if (contentLength > 0 && totalRead >= contentLength) break;
                        }
                    } else if (!client.connected()) {
                        break;
                    } else {
                        delay(1);
                    }
                }
                Serial.printf("[DAV] non-chunked read: got %d / %d bytes\n", totalRead, contentLength);
            }
        } else {
            timeout = millis() + 3000;
            while ((client.connected() || client.available()) && millis() < timeout) {
                if (client.available()) result += client.readString();
                else delay(5);
            }
            Serial.println("[DAV] Error response: " + result);
        }

        Serial.printf("[DAV] result.length()=%u\n", result.length());
        client.stop();
        return httpCode;
    }

    // 流式 PROPFIND：读到数据就即时解析，不需要存储完整 XML
    int propfindStream(const char* relativePath, std::function<void(const String&)> onBlock) {
        WiFiClientSecure client;
        client.setInsecure();
        if (!client.connect("dav.jianguoyun.com", 443)) {
            Serial.println("[DAV] stream: connect failed");
            return 0;
        }

        // 构建 URL 编码请求路径（与 propfind 相同逻辑）
        String requestPath = "/dav/";
        if (relativePath && strlen(relativePath) > 0) {
            String pathStr = String(relativePath);
            int startPos = 0;
            bool first = true;
            while (true) {
                int slashPos = pathStr.indexOf('/', startPos);
                String segment = (slashPos == -1) ? pathStr.substring(startPos) : pathStr.substring(startPos, slashPos);
                if (segment.length() > 0) {
                    if (!first) requestPath += "/";
                    requestPath += String(urlEncode(segment.c_str()).c_str());
                    first = false;
                }
                if (slashPos == -1) break;
                startPos = slashPos + 1;
            }
        }
        if (!requestPath.endsWith("/")) requestPath += "/";
        Serial.printf("[DAV] PROPFIND %s\n", requestPath.c_str());

        String authStr = _user + ":" + _pass;
        String base64Auth = base64::encode(authStr);
        base64Auth.trim();

        client.print("PROPFIND "); client.print(requestPath); client.println(" HTTP/1.1");
        client.println("Host: dav.jianguoyun.com");
        client.println("Authorization: Basic " + base64Auth);
        client.println("Depth: 1");
        client.println("Content-Length: 0");
        client.println("Connection: close");
        client.println();

        unsigned long timeout = millis() + 8000;
        auto readLine = [&]() -> String {
            String line;
            unsigned long t = millis() + 3000;
            while (millis() < t) {
                if (client.available()) {
                    char ch = client.read();
                    if (ch == '\n') return line;
                    if (ch != '\r') line += ch;
                } else delay(1);
            }
            return line;
        };

        int httpCode = 0;
        String statusLine = readLine();
        if (statusLine.startsWith("HTTP/1.1 ") || statusLine.startsWith("HTTP/1.0 ")) {
            httpCode = statusLine.substring(9, 12).toInt();
        } else {
            client.stop();
            return 0;
        }

        bool isChunked = false;
        int contentLength = -1;
        while (millis() < timeout) {
            String hdr = readLine();
            if (hdr.length() == 0) break;
            String lower = hdr; lower.toLowerCase();
            if (lower.startsWith("transfer-encoding:") && lower.indexOf("chunked") != -1) isChunked = true;
            else if (lower.startsWith("content-length:")) contentLength = lower.substring(15).toInt();
        }

        if (httpCode == 207) {
            // 滚动缓冲区：只需保存当前未处理的数据，最大不超过一个响应块的大小（~1KB）
            String buf;
            buf.reserve(2048);

            // 从 buf 中提取完整的 <d:response>...</d:response> 块并回调
            auto flushBlocks = [&]() {
                while (true) {
                    int s = buf.indexOf("<d:response>");
                    if (s == -1) s = buf.indexOf("<D:response>");
                    if (s == -1) {
                        // 未找到开始标签：保留末尾 11 字节（防止标签被切割）
                        if ((int)buf.length() > 11) buf = buf.substring(buf.length() - 11);
                        return;
                    }
                    if (s > 0) buf = buf.substring(s);  // 丢弃开始标签前的内容
                    int e = buf.indexOf("</d:response>");
                    if (e == -1) e = buf.indexOf("</D:response>");
                    if (e == -1) return;  // 块不完整，等待更多数据
                    onBlock(buf.substring(12, e));  // 12 = len("<d:response>")
                    buf = buf.substring(e + 13);    // 13 = len("</d:response>")
                }
            };

            timeout = millis() + 10000;
            int totalRead = 0;
            if (isChunked) {
                while (millis() < timeout) {
                    String szLine = readLine(); szLine.trim();
                    if (szLine.length() == 0) continue;
                    int chunkSize = (int)strtol(szLine.c_str(), nullptr, 16);
                    if (chunkSize == 0) break;
                    int rem = chunkSize;
                    unsigned long ct = millis() + 3000;
                    while (rem > 0 && millis() < ct) {
                        if (client.available()) {
                            char tmp[128];
                            int r = rem < (int)sizeof(tmp) ? rem : (int)sizeof(tmp);
                            int got = client.read((uint8_t*)tmp, r);
                            if (got > 0) { buf.concat(tmp, got); rem -= got; ct = millis() + 3000; }
                        } else delay(1);
                    }
                    readLine();  // chunk 末尾 \r\n
                    flushBlocks();
                    timeout = millis() + 10000;
                }
            } else {
                while (millis() < timeout) {
                    if (client.available()) {
                        char tmp[256];
                        int toRead = (int)sizeof(tmp);
                        if (contentLength > 0) {
                            int rem = contentLength - totalRead;
                            if (rem <= 0) break;
                            if (toRead > rem) toRead = rem;
                        }
                        int got = client.read((uint8_t*)tmp, toRead);
                        if (got > 0) {
                            buf.concat(tmp, got);
                            totalRead += got;
                            timeout = millis() + 5000;
                            flushBlocks();
                            if (contentLength > 0 && totalRead >= contentLength) break;
                        }
                    } else if (!client.connected()) {
                        break;
                    } else delay(1);
                }
            }
            flushBlocks();  // 处理剩余数据
        }

        client.stop();
        return httpCode;
    }

    void end() {}

private:
    String _baseUrl;
    String _user;
    String _pass;
    int _timeout;
    String _basePath;  // 新增：自定义基础路径
};
// ===================== 内嵌ESPWebDAV核心代码（结束）=====================

namespace {
constexpr int PAGE_ITEMS = 23;
constexpr int SKIP_PAGE_MS = 700;
}  // namespace

// 静态成员变量初始化
std::vector<WebDAVEntry> JianGuoBrowserActivity::entries;
std::string JianGuoBrowserActivity::currentPath = "";
std::vector<std::string> JianGuoBrowserActivity::navigationHistory;
int JianGuoBrowserActivity::selectorIndex = 0;
std::string JianGuoBrowserActivity::errorMessage;
std::string JianGuoBrowserActivity::statusMessage;
JianGuoBrowserActivity::BrowserState JianGuoBrowserActivity::state;
SemaphoreHandle_t JianGuoBrowserActivity::renderingMutex = nullptr;
TaskHandle_t JianGuoBrowserActivity::displayTaskHandle = nullptr;
bool JianGuoBrowserActivity::updateRequired = false;

// 修复：把endsWith的定义放在这里（头文件只留声明）
bool JianGuoBrowserActivity::endsWith(const std::string& str, const std::string& suffix) {
    if (str.length() < suffix.length()) return false;
    return str.compare(str.length() - suffix.length(), suffix.length(), suffix) == 0;
}

void JianGuoBrowserActivity::taskTrampoline(void* param) {
  auto* self = static_cast<JianGuoBrowserActivity*>(param);
  self->displayTaskLoop();
}

void JianGuoBrowserActivity::onEnter() {
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

  xTaskCreate(&JianGuoBrowserActivity::taskTrampoline, "JianGuoBookBrowserTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );

  // 检查WiFi后加载目录
  autoConnectAttempted = false;
  autoConnectStartTime = 0;
  checkAndConnectWifi();
}

void JianGuoBrowserActivity::onExit() {
  ActivityWithSubactivity::onExit();

  // 重置自动连接状态
  autoConnectAttempted = false;

  // 关闭WiFi
  WiFi.mode(WIFI_OFF);

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
  entries.clear();
  navigationHistory.clear();
}

void JianGuoBrowserActivity::loop() {
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

  // 检查WiFi/加载中：仅返回键可用
  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    // 如果正在自动连接，检查连接进度
    if (state == BrowserState::CHECK_WIFI && autoConnectAttempted) {
      checkAutoConnectProgress();
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) || touchBack()) {
      // 用户取消自动连接
      if (autoConnectAttempted) {
        autoConnectAttempted = false;
        WiFi.disconnect();
      }
      exitActivity(); 
    }
    return;
  }

  // 浏览目录状态（核心）
  if (state == BrowserState::BROWSING) {
    const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up);
    const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down);
    const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;
    const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, false);

    // Touch: same drawList geometry as render()
    if (mappedInput.hasTouch()) {
      auto metrics = UITheme::getInstance().getMetrics();
      const int pageHeight = renderer.getScreenHeight();
      const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
      const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
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
        return;
      }
      if (act == M4ListTouchPolicy::Action::PageDown || act == M4ListTouchPolicy::Action::PageUp) {
        if (itemCount > 0) {
          selectorIndex = M4ListTouchPolicy::applyPage(selectorIndex, itemCount, pageItems,
                                                       act == M4ListTouchPolicy::Action::PageDown);
          updateRequired = true;
        }
        return;
      }
      if (act == M4ListTouchPolicy::Action::Select && hit >= 0) {
        if (selectorIndex != hit) {
          selectorIndex = hit;
          updateRequired = true;
        }
        return;
      }
      if (act == M4ListTouchPolicy::Action::Activate && hit >= 0) {
        selectorIndex = hit;
        const auto& entry = entries[selectorIndex];
        if (entry.type == WebDAVEntry::FOLDER) navigateToEntry(entry);
        else if (entry.type == WebDAVEntry::BOOK_FILE) downloadBook(entry);
        return;
      }
    }

    // 确认键：打开文件夹（跳过下载逻辑）
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        if (!entries.empty()) {
            const auto& entry = entries[selectorIndex];
            if (entry.type == WebDAVEntry::FOLDER) {
                navigateToEntry(entry); // 进入文件夹
            } else if (entry.type == WebDAVEntry::BOOK_FILE) {
                downloadBook(entry); // ← 新增：下载文件
            }
        }
    }
    // 左键：刷新当前目录
    else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      state = BrowserState::LOADING;
      statusMessage = L(Str::kRefreshing);
      updateRequired = true;
      fetchFeed(currentPath);
    }
    // 返回键：返回上一级
    else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      if (navigationHistory.empty()) {
        onGoHome();
      } else {
        navigateBack();
      }
    } 
    // 上下键：切换选中项
    else if (prevReleased && !entries.empty()) {
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

void JianGuoBrowserActivity::displayTaskLoop() {
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

void JianGuoBrowserActivity::drawLocalButtonHints(const char* btn1, const char* btn2, const char* btn3, const char* btn4) const {
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

void JianGuoBrowserActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getInstance().getMetrics();

  // 标题栏：显示当前路径
  std::string pathDisplay = currentPath.empty() ? L(Str::kJianGuo) : currentPath;
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
    // 列表为空时，不显示"无文件"，保持空白
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
          return entries[index].type == WebDAVEntry::FOLDER ? UIIcon::Folder : UITheme::getFileIcon(entries[index].title);
        },
        [this](int index) -> std::string {
          // 返回文件大小
          if (entries[index].fileSize == 0) return "";
          char buf[16];
          uint32_t bytes = entries[index].fileSize;
          if (bytes >= 1024u * 1024u) {
            snprintf(buf, sizeof(buf), "%.1fMB", bytes / (1024.0f * 1024.0f));
          } else if (bytes >= 1024u) {
            snprintf(buf, sizeof(buf), "%uKB", static_cast<unsigned>(bytes / 1024u));
          } else {
            snprintf(buf, sizeof(buf), "%uB", static_cast<unsigned>(bytes));
          }
          return buf;
        });
  }

  // 按钮提示（始终显示，force=true强制显示不受系统设置影响）
  // 根据选中的是目录还是文件动态显示按钮文字
  const char* confirmLabel = L(Str::kOpen);  // 默认显示"打开"
  if (!entries.empty() && selectorIndex >= 0 && selectorIndex < static_cast<int>(entries.size())) {
    if (entries[selectorIndex].type == WebDAVEntry::FOLDER) {
      confirmLabel = L(Str::kOpen);
    } else {
      confirmLabel = L(Str::kDownload);
    }
  }
  const auto labels = mappedInput.mapLabels(L(Str::kBack), confirmLabel, L(Str::kRefresh), L(Str::kNextPage));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);

  renderer.displayBuffer();
}

// 核心：列目录逻辑（支持逐级目录浏览）
void JianGuoBrowserActivity::fetchFeed(const std::string& subPath) {
    std::string username = SETTINGS.jgUsername;
    std::string appPwd = SETTINGS.jgAppPassword;

    if (username.empty() || appPwd.empty()) {
        state = BrowserState::ERROR;
        errorMessage = L(Str::kPleaseConfigUserPass);
        updateRequired = true;
        return;
    }

    entries.clear();

    ESPWebDAV dav;
    dav.begin("https://dav.jianguoyun.com/dav/", username.c_str(), appPwd.c_str());
    dav.setTimeout(8000);

    // 流式解析：每个 <d:response> 块寻到就即时解析，无需缓存完整 XML
    int responseCode = dav.propfindStream(subPath.c_str(), [&](const String& block) {
        parseResponseBlock(block, subPath, "jg", entries);
    });

    if (responseCode != 207) {
        state = BrowserState::ERROR;
        if (responseCode == 401) {
            errorMessage = L(Str::kUserPassError);
        } else if (responseCode == 403) {
            errorMessage = L(Str::kAccessDenied);
        } else {
            char buf[32];
            snprintf(buf, sizeof(buf), "HTTP %d", responseCode);
            errorMessage = buf;
        }
        updateRequired = true;
        return;
    }

    dav.end();
    selectorIndex = 0;
    state = BrowserState::BROWSING;
    updateRequired = true;
}

// 解析单个 <d:response> 块并将文件条目添加到 outEntries
void JianGuoBrowserActivity::parseResponseBlock(const String& responseBlock,
                                                 const std::string& basePath,
                                                 const std::string& sourceFolder,
                                                 std::vector<WebDAVEntry>& outEntries) {
    // 提取 href
    int hrefStart = responseBlock.indexOf("<d:href>");
    if (hrefStart == -1) hrefStart = responseBlock.indexOf("<D:href>");
    if (hrefStart == -1) return;
    hrefStart += 8;
    int hrefEnd = responseBlock.indexOf("</d:href>", hrefStart);
    if (hrefEnd == -1) hrefEnd = responseBlock.indexOf("</D:href>", hrefStart);
    if (hrefEnd == -1) return;
    String href = responseBlock.substring(hrefStart, hrefEnd);

    // 提取 displayname
    int nameStart = responseBlock.indexOf("<d:displayname>");
    if (nameStart == -1) nameStart = responseBlock.indexOf("<D:displayname>");
    if (nameStart == -1) return;
    nameStart += 15;
    int nameEnd = responseBlock.indexOf("</d:displayname>", nameStart);
    if (nameEnd == -1) nameEnd = responseBlock.indexOf("</D:displayname>", nameStart);
    if (nameEnd == -1) return;
    std::string fileName = responseBlock.substring(nameStart, nameEnd).c_str();

    if (fileName == "." || fileName == ".." || fileName.empty()) return;

    // 过滤当前目录本身
    String currentDirHref = "/dav/";
    if (!basePath.empty()) {
        String pathStr = String(basePath.c_str());
        int startPos = 0;
        bool first = true;
        while (true) {
            int slashPos = pathStr.indexOf('/', startPos);
            String segment = (slashPos == -1) ? pathStr.substring(startPos) : pathStr.substring(startPos, slashPos);
            if (segment.length() > 0) {
                if (!first) currentDirHref += "/";
                currentDirHref += String(urlEncode(segment.c_str()).c_str());
                first = false;
            }
            if (slashPos == -1) break;
            startPos = slashPos + 1;
        }
    }
    String hrefTrim = href;
    if (hrefTrim.endsWith("/")) hrefTrim = hrefTrim.substring(0, hrefTrim.length() - 1);
    String curTrim = currentDirHref;
    if (curTrim.endsWith("/")) curTrim = curTrim.substring(0, curTrim.length() - 1);
    if (hrefTrim == curTrim) return;  // 跳过目录本身

    bool isFolder = (responseBlock.indexOf("<d:collection") != -1 ||
                     responseBlock.indexOf("<D:collection") != -1);

    uint32_t fileSize = 0;
    if (!isFolder) {
        int sizeStart = responseBlock.indexOf("<d:getcontentlength>");
        if (sizeStart == -1) sizeStart = responseBlock.indexOf("<D:getcontentlength>");
        if (sizeStart != -1) {
            sizeStart += 20;
            int sizeEnd = responseBlock.indexOf("</d:getcontentlength>", sizeStart);
            if (sizeEnd == -1) sizeEnd = responseBlock.indexOf("</D:getcontentlength>", sizeStart);
            if (sizeEnd != -1) fileSize = responseBlock.substring(sizeStart, sizeEnd).toInt();
        }
    }

    WebDAVEntry entry;
    entry.title = fileName;
    entry.path = basePath.empty() ? fileName : basePath + "/" + fileName;
    entry.fileSize = fileSize;
    entry.type = isFolder ? WebDAVEntry::FOLDER : WebDAVEntry::BOOK_FILE;
    entry.sourceFolder = sourceFolder;
    outEntries.push_back(entry);
}

// 文件夹跳转逻辑
void JianGuoBrowserActivity::navigateToEntry(const WebDAVEntry& entry) {
  if (entry.type != WebDAVEntry::FOLDER) return;

  // 记录历史路径
  navigationHistory.push_back(currentPath);
  currentPath = entry.path;

  // 加载子目录
  state = BrowserState::LOADING;
  statusMessage = L(Str::kLoading);
  entries.clear();
  selectorIndex = 0;
  updateRequired = true;

  fetchFeed(currentPath);
}

// 返回上一级目录
void JianGuoBrowserActivity::navigateBack() {
  if (navigationHistory.empty()) {
    exitActivity(); // 根目录返回直接退出
  } else {
    // 回到上一级
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

// WiFi检查逻辑
void JianGuoBrowserActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    state = BrowserState::LOADING;
    statusMessage = L(Str::kLoading);
    updateRequired = true;
    fetchFeed(currentPath);
    return;
  }

  // 加载已保存的WiFi凭据（SD卡和显示共用SPI总线，需要互斥）
  if (renderingMutex) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      WIFI_STORE.loadFromFile();
      xSemaphoreGive(renderingMutex);
  } else {
      WIFI_STORE.loadFromFile();
  }
  const auto& credentials = WIFI_STORE.getCredentials();

  if (credentials.empty() || SETTINGS.wifiAlwaysReselect) {
    // 没有保存的凭据或用户设置了每次重新选择WiFi
    Serial.printf("[%lu] [JG] 无已保存的WiFi凭据或每次重选WiFi，跳转选择页面\n", millis());
    launchWifiSelection();
    return;
  }

  // 有保存的凭据，尝试自动连接第一个
  const auto& cred = credentials[0];
  Serial.printf("[%lu] [JG] 尝试自动连接: %s\n", millis(), cred.ssid.c_str());

  // 禁用蓝牙（ESP32-C3上WiFi和蓝牙互斥）
  try {
    auto& btMgr = BluetoothHIDManager::getInstance();
    if (btMgr.isEnabled()) {
      btMgr.disable();
    }
  } catch (...) {}

  // 开始连接WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(cred.ssid.c_str(), cred.password.c_str());

  // 设置自动连接状态
  state = BrowserState::CHECK_WIFI;
  autoConnectAttempted = true;
  autoConnectStartTime = millis();
  statusMessage = std::string(L(Str::kConnectingTo)) + cred.ssid + "...";
  updateRequired = true;
}

// 检查自动连接进度
void JianGuoBrowserActivity::checkAutoConnectProgress() {
  if (!autoConnectAttempted) return;

  wl_status_t status = WiFi.status();

  if (status == WL_CONNECTED) {
    // 连接成功
    Serial.printf("[%lu] [JG] 自动连接成功: %s\n", millis(), WiFi.localIP().toString().c_str());
    autoConnectAttempted = false;
    state = BrowserState::LOADING;
    statusMessage = L(Str::kLoading);
    updateRequired = true;
    fetchFeed(currentPath);
    return;
  }

  if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
    // 连接失败
    Serial.printf("[%lu] [JG] 自动连接失败: status=%d\n", millis(), status);
    autoConnectAttempted = false;
    WiFi.disconnect();
    launchWifiSelection();
    return;
  }

  // 检查超时（10秒）
  if (millis() - autoConnectStartTime > 10000) {
    Serial.printf("[%lu] [JG] 自动连接超时\n", millis());
    autoConnectAttempted = false;
    WiFi.disconnect();
    launchWifiSelection();
    return;
  }
}

// 启动WiFi选择页面
void JianGuoBrowserActivity::launchWifiSelection() {
  state = BrowserState::WIFI_SELECTION;
  updateRequired = true;

  enterNewActivity(new WifiSelectionActivity(renderer, mappedInput,
                                             [this](const bool connected) { onWifiSelectionComplete(connected); }));
}

// WiFi选择完成回调
void JianGuoBrowserActivity::onWifiSelectionComplete(const bool connected) {
  exitActivity();

  if (connected) {
    Serial.printf("[%lu] [JG] WiFi已连接，加载目录\n", millis());
    state = BrowserState::LOADING;
    statusMessage = L(Str::kLoading);
    updateRequired = true;
    fetchFeed(currentPath);
  } else {
    Serial.printf("[%lu] [JG] WiFi连接失败\n", millis());
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
    state = BrowserState::ERROR;
    errorMessage = L(Str::kWifiConnectFailed);
    updateRequired = true;
  }
}


void JianGuoBrowserActivity::downloadBook(const WebDAVEntry& book) {
    state = BrowserState::DOWNLOADING;
    statusMessage = book.title;
    downloadProgress = 0;
    downloadTotal = 0;
    updateRequired = true;

    // === 构建下载 URL ===
    std::string downloadUrl = "https://dav.jianguoyun.com/dav/";
    
    // 添加文件路径并编码
    downloadUrl += urlEncode(book.path);
    Serial.printf("[%lu] [JG] 准备下载: %s (来源:%s)\n", millis(), 
                  downloadUrl.c_str(), book.sourceFolder.c_str());

    // === 根据扩展名选择本地保存目录 ===
    std::string targetDir;
    if (endsWith(book.title, ".pngtxt")) {
        targetDir = "/lock_screen/";
    } else if (endsWith(book.title, ".epdfont")) {
        targetDir = "/fonts";
    } else {
        targetDir = "/坚果云";
    }

    if (!targetDir.empty()) {
        SdMan.mkdir(targetDir.c_str());
    }

    std::string safeFilename = StringUtils::sanitizeFilename(book.title,200);
    std::string localPath;
    if (targetDir.empty()) {
        localPath = "/" + safeFilename;
    } else {
        localPath = targetDir + "/" + safeFilename;
    }

    Serial.printf("[%lu] [JG] Downloading: %s -> %s\n", millis(), 
                  downloadUrl.c_str(), localPath.c_str());

    const auto result = HttpDownloader::downloadToFile_jg(
        downloadUrl,
        localPath,
        [this](size_t downloaded, size_t total) {
            downloadProgress = downloaded;
            downloadTotal = total;
            updateRequired = true;
        }
    );

    if (result == HttpDownloader::OK) {
        Serial.printf("[%lu] [JG] 下载完成: %s\n", millis(), localPath.c_str());

        // 下载成功后删除云盘文件
        // 构建与下载时相同的路径
        std::string deletePath;
        if (book.sourceFolder == "legado") {
            deletePath = "legado/books/";
        } else {
            deletePath = std::string(SETTINGS.jgBookFolder) + "/";
        }
        
        // 添加子路径（与下载时一致）
        std::string deleteFolderPath = book.path;
        size_t deleteLastSlash = deleteFolderPath.rfind('/');
        if (deleteLastSlash != std::string::npos && deleteLastSlash > 0) {
            deleteFolderPath = deleteFolderPath.substr(0, deleteLastSlash);
            deletePath += deleteFolderPath + "/";
        }
        deletePath += urlEncode(book.title);

        Serial.printf("[%lu] [JG] 准备删除云盘文件：%s\n", millis(), deletePath.c_str());

        ESPWebDAV dav;
        dav.begin("https://dav.jianguoyun.com/dav", SETTINGS.jgUsername, SETTINGS.jgAppPassword);
        dav.setBasePath("__FULL_PATH__");  // 标记：deletePath 已是完整路径
        int deleteCode = dav.deleteFile(deletePath.c_str());
        if (deleteCode == 204) {
            Serial.printf("[%lu] [JG] 已删除云盘文件：%s\n", millis(), deletePath.c_str());
            // 从本地列表中移除该条目
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                if (it->title == book.title && it->path == book.path) {
                    entries.erase(it);
                    Serial.printf("[%lu] [JG] 已从列表移除：%s\n", millis(), book.title.c_str());
                    break;
                }
            }
        } else {
            Serial.printf("[%lu] [JG] 删除云盘文件失败：%d\n", millis(), deleteCode);
        }

        if (endsWith(book.title, ".epub")) {
            Epub epub(localPath, "/.crosspoint");
            epub.clearCache();
        }

        state = BrowserState::BROWSING;
        updateRequired = true;
    } else {
        state = BrowserState::ERROR;
        errorMessage = "下载失败";
        updateRequired = true;
    }
}