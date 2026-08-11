#include "OtaManager.h"

#include <ArduinoJson.h>
#include <HardwareSerial.h>
#include <MD5Builder.h>
#include <SDCardManager.h>
#include <WiFi.h>

#include "HttpDownloader.h"
#include "GbkToUtf8.h"

// ---- 纯函数（可单元测试） ----

bool OtaManager::parseReleaseJson(const std::string& json,
                                   const std::string& hardwareSuffix,
                                   std::string& outTagName,
                                   std::string& outBody,
                                   std::string& outFirmwareUrl,
                                   std::string& outMd5Url,
                                   std::string& outUpdateTxtUrl) {
  outTagName.clear();
  outBody.clear();
  outFirmwareUrl.clear();
  outMd5Url.clear();
  outUpdateTxtUrl.clear();

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, json);
  if (error) {
    return false;
  }

  // 提取 tag_name（必需）
  if (!doc["tag_name"].is<const char*>()) {
    return false;
  }
  outTagName = doc["tag_name"].as<const char*>();

  // 提取 body（可选，允许为空字符串）
  if (doc["body"].is<const char*>()) {
    outBody = doc["body"].as<const char*>();
  }

  // 提取 assets 数组（必需）
  if (!doc["assets"].is<JsonArray>()) {
    return false;
  }
  JsonArray assets = doc["assets"].as<JsonArray>();

  // 构造期望的文件名
  std::string expectedFirmware = "firmware-" + hardwareSuffix + ".bin";
  std::string expectedMd5 = "md5-" + hardwareSuffix + ".txt";

  bool foundFirmware = false;
  bool foundMd5 = false;

  for (JsonObject asset : assets) {
    if (!asset["name"].is<const char*>() || !asset["browser_download_url"].is<const char*>()) {
      continue;
    }
    std::string name = asset["name"].as<const char*>();
    std::string url = asset["browser_download_url"].as<const char*>();

    if (name == expectedFirmware) {
      outFirmwareUrl = url;
      foundFirmware = true;
    } else if (name == expectedMd5) {
      outMd5Url = url;
      foundMd5 = true;
    } else if (name == "update.log") {
      outUpdateTxtUrl = url;
    }
  }

  return foundFirmware && foundMd5;
}

bool OtaManager::parseTagVersion(const std::string& tagName, int& outVersion) {
  if (tagName.empty()) {
    return false;
  }

  const char* str = tagName.c_str();

  // 跳过可选的 v/V 前缀
  if (*str == 'v' || *str == 'V') {
    str++;
  }

  // 剩余部分不能为空
  if (*str == '\0') {
    return false;
  }

  // 用 strtol 提取整数
  char* end = nullptr;
  long val = strtol(str, &end, 10);

  // 确保整个剩余字符串都是数字（end 指向 '\0'）
  if (end == str || *end != '\0') {
    return false;
  }

  outVersion = static_cast<int>(val);
  return true;
}

bool OtaManager::isNewerVersion(int remoteVersion, int localVersion) {
  return remoteVersion > localVersion;
}

// ---- 版本检查 ----

OtaManager::Error OtaManager::checkForUpdate() {
  // 1. 检查 WiFi 连接状态
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[%lu] [OTA] WiFi not connected\n", millis());
    return WIFI_NOT_CONNECTED;
  }

  // 2. 调用 Gitee Release API 获取 JSON
  std::string content;
  Serial.printf("[%lu] [OTA] Fetching release info from: %s\n", millis(), RELEASE_API_URL);
  if (!HttpDownloader::fetchUrl(RELEASE_API_URL, content)) {
    Serial.printf("[%lu] [OTA] Failed to fetch release info\n", millis());
    return HTTP_ERROR;
  }

  // 3. 确定硬件后缀
#ifdef CROSSPOINT_X3
  const std::string hardwareSuffix = "x3";
#else
  const std::string hardwareSuffix = "x4";
#endif

  // 4. 解析 Release JSON
  std::string tagName, body, firmwareUrl, md5Url, updateTxtUrl;
  if (!parseReleaseJson(content, hardwareSuffix, tagName, body, firmwareUrl, md5Url, updateTxtUrl)) {
    Serial.printf("[%lu] [OTA] Failed to parse release JSON\n", millis());
    return PARSE_ERROR;
  }

  // 5. 从 tag_name 提取远程版本号
  int remoteVersion = 0;
  if (!parseTagVersion(tagName, remoteVersion)) {
    Serial.printf("[%lu] [OTA] Failed to parse tag version: %s\n", millis(), tagName.c_str());
    return PARSE_ERROR;
  }

  // 6. 从 CROSSPOINT_VERSION 宏提取本地版本号
  int localVersion = atoi(CROSSPOINT_VERSION);

  // 7. 填充 updateInfo
  updateInfo.remoteVersion = remoteVersion;
  updateInfo.localVersion = localVersion;
  updateInfo.firmwareUrl = firmwareUrl;
  updateInfo.md5Url = md5Url;
  updateInfo.updateTxtUrl = updateTxtUrl;

  Serial.printf("[%lu] [OTA] Remote version: %d, Local version: %d\n", millis(), remoteVersion, localVersion);

  // 8-9. 版本对比
  if (isNewerVersion(remoteVersion, localVersion)) {
    Serial.printf("[%lu] [OTA] Update available\n", millis());

    // 10. 下载 update.txt 获取更新说明（优先使用 assets 中的 update.txt，回退到 Release body）
    if (!updateTxtUrl.empty()) {
      std::string updateTxtContent;
      if (HttpDownloader::fetchUrl(updateTxtUrl, updateTxtContent)) {
        // 检测编码，如果不是 UTF-8 则从 GBK 转换
        if (!updateTxtContent.empty() &&
            !isValidUtf8(reinterpret_cast<const uint8_t*>(updateTxtContent.data()),
                         updateTxtContent.size())) {
          Serial.printf("[%lu] [OTA] update.txt is GBK, converting to UTF-8\n", millis());
          uint8_t carry = 0;
          bool hasCarry = false;
          updateInfo.remark = gbkChunkToUtf8(
              reinterpret_cast<const uint8_t*>(updateTxtContent.data()),
              updateTxtContent.size(), getGbkTable(), carry, hasCarry);
        } else {
          updateInfo.remark = updateTxtContent;
        }
        Serial.printf("[%lu] [OTA] Loaded update description from update.txt\n", millis());
      } else {
        // 下载失败，回退到 Release body
        updateInfo.remark = body;
        Serial.printf("[%lu] [OTA] Failed to fetch update.txt, using release body\n", millis());
      }
    } else {
      // 没有 update.txt，使用 Release body
      updateInfo.remark = body;
    }

    return OK;
  }

  Serial.printf("[%lu] [OTA] Already up to date\n", millis());
  return NO_UPDATE;
}

// ---- 下载、校验、刷机 ----

OtaManager::Error OtaManager::downloadFirmware(ProgressCallback progress) {
  // 确保 /update/ 目录存在
  if (!SdMan.exists(SD_UPDATE_DIR)) {
    if (!SdMan.mkdir(SD_UPDATE_DIR)) {
      Serial.printf("[%lu] [OTA] Failed to create %s directory\n", millis(), SD_UPDATE_DIR);
      return FILE_ERROR;
    }
  }

  // 下载 firmware.bin（使用 updateInfo 中的 URL）
  Serial.printf("[%lu] [OTA] Downloading firmware: %s\n", millis(), updateInfo.firmwareUrl.c_str());

  auto result = HttpDownloader::downloadToFile(updateInfo.firmwareUrl, SD_FIRMWARE_PATH, progress);
  if (result != HttpDownloader::OK) {
    Serial.printf("[%lu] [OTA] Failed to download firmware.bin: %d\n", millis(), result);
    cleanupDownloadedFiles();
    return (result == HttpDownloader::FILE_ERROR) ? FILE_ERROR : HTTP_ERROR;
  }

  // 下载 md5.txt（使用 updateInfo 中的 URL）
  Serial.printf("[%lu] [OTA] Downloading md5: %s\n", millis(), updateInfo.md5Url.c_str());

  result = HttpDownloader::downloadToFile(updateInfo.md5Url, SD_MD5_PATH);
  if (result != HttpDownloader::OK) {
    Serial.printf("[%lu] [OTA] Failed to download md5.txt: %d\n", millis(), result);
    cleanupDownloadedFiles();
    return (result == HttpDownloader::FILE_ERROR) ? FILE_ERROR : HTTP_ERROR;
  }

  Serial.printf("[%lu] [OTA] Download complete\n", millis());
  return OK;
}

OtaManager::Error OtaManager::verifyMd5() {
  // 读取 md5.txt 中的预期 MD5 值
  String md5Content = SdMan.readFile(SD_MD5_PATH);
  if (md5Content.isEmpty()) {
    Serial.printf("[%lu] [OTA] Failed to read md5.txt\n", millis());
    cleanupDownloadedFiles();
    return FILE_ERROR;
  }

  // 去掉前后空白字符，提取 32 字符的 MD5 哈希
  md5Content.trim();
  std::string expectedMd5 = md5Content.c_str();
  Serial.printf("[%lu] [OTA] Expected MD5: %s\n", millis(), expectedMd5.c_str());

  // 打开 firmware.bin 并计算 MD5
  FsFile file;
  if (!SdMan.openFileForRead("OTA", SD_FIRMWARE_PATH, file)) {
    Serial.printf("[%lu] [OTA] Failed to open firmware.bin for MD5 check\n", millis());
    cleanupDownloadedFiles();
    return FILE_ERROR;
  }

  MD5Builder md5;
  md5.begin();

  uint8_t buffer[1024];
  size_t totalRead = 0;
  while (true) {
    size_t bytesRead = file.read(buffer, sizeof(buffer));
    if (bytesRead == 0) break;
    md5.add(buffer, bytesRead);
    totalRead += bytesRead;
  }
  file.close();

  md5.calculate();
  std::string calculatedMd5 = md5.toString().c_str();
  Serial.printf("[%lu] [OTA] Calculated MD5: %s (from %zu bytes)\n", millis(), calculatedMd5.c_str(), totalRead);

  // 对比 MD5（不区分大小写）
  if (!expectedMd5.empty() && !calculatedMd5.empty()) {
    // 转为小写比较
    std::string expectedLower = expectedMd5;
    std::string calculatedLower = calculatedMd5;
    for (auto& c : expectedLower) c = tolower(c);
    for (auto& c : calculatedLower) c = tolower(c);

    if (expectedLower == calculatedLower) {
      Serial.printf("[%lu] [OTA] MD5 verification passed\n", millis());
      return OK;
    }
  }

  Serial.printf("[%lu] [OTA] MD5 mismatch!\n", millis());
  cleanupDownloadedFiles();
  return MD5_MISMATCH;
}

OtaManager::Error OtaManager::flashFirmware(SdOtaUpdater::ProgressCallback progress) {
  // 先检查 SD 卡上的固件文件
  auto checkResult = sdUpdater.checkSdCard();
  if (checkResult != SdOtaUpdater::OK) {
    Serial.printf("[%lu] [OTA] SD card check failed: %d\n", millis(), checkResult);
    return (checkResult == SdOtaUpdater::NO_UPDATE) ? FILE_ERROR : FLASH_ERROR;
  }

  Serial.printf("[%lu] [OTA] Starting flash, firmware size: %zu bytes\n", millis(), sdUpdater.getFirmwareSize());

  // 调用 SdOtaUpdater 执行刷机
  auto flashResult = sdUpdater.flashUpdaterAndReboot(progress);
  if (flashResult != SdOtaUpdater::OK) {
    Serial.printf("[%lu] [OTA] Flash failed: %d\n", millis(), flashResult);
    return FLASH_ERROR;
  }

  Serial.printf("[%lu] [OTA] Flash complete, reboot required\n", millis());
  return OK;
}

void OtaManager::cleanupDownloadedFiles() {
  if (SdMan.exists(SD_FIRMWARE_PATH)) {
    SdMan.remove(SD_FIRMWARE_PATH);
    Serial.printf("[%lu] [OTA] Removed %s\n", millis(), SD_FIRMWARE_PATH);
  }
  if (SdMan.exists(SD_MD5_PATH)) {
    SdMan.remove(SD_MD5_PATH);
    Serial.printf("[%lu] [OTA] Removed %s\n", millis(), SD_MD5_PATH);
  }
}
