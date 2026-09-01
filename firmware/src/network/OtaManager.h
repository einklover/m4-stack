#pragma once

#include <functional>
#include <string>

class OtaManager {
 public:
  enum Error {
    OK = 0,
    HTTP_ERROR,          // 网络请求失败
    PARSE_ERROR,         // JSON 解析失败
    NO_UPDATE,           // 已是最新版本
    FILE_ERROR,          // SD 卡文件操作失败
    MD5_MISMATCH,        // MD5 校验失败
    FLASH_ERROR,         // 刷机失败
    WIFI_NOT_CONNECTED,  // WiFi 未连接
  };

  struct UpdateInfo {
    int remoteVersion = 0;
    int localVersion = 0;
    std::string remark;        // 更新备注（来自 assets 中的 update.txt）
    std::string firmwareUrl;   // firmware.bin 的下载链接（来自 assets）
    std::string md5Url;        // md5.txt 的下载链接（来自 assets）
    std::string updateTxtUrl;  // update.txt 的下载链接（来自 assets，可选）
  };

  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;

  OtaManager() = default;

  /// 从 Gitee Release API 检查更新，填充 updateInfo
  Error checkForUpdate();

  /// 下载 firmware.bin 和 md5.txt 到 /update/ 目录
  Error downloadFirmware(ProgressCallback progress = nullptr);

  /// 校验已下载的 firmware.bin 的 MD5
  Error verifyMd5();

  /// 将已校验的固件直接写入官方 app0（刷机完成后需重启）
  Error flashFirmware(ProgressCallback progress = nullptr);

  /// 清理 /update/ 目录中的下载文件
  void cleanupDownloadedFiles();

  const UpdateInfo& getUpdateInfo() const { return updateInfo; }

  // ---- 可单元测试的纯函数 ----

  /// 解析 Gitee Release API 的 JSON 响应
  /// 提取 tag_name、body、assets 中对应硬件版本的固件和 MD5 下载链接
  /// @param json JSON 响应字符串
  /// @param hardwareSuffix 硬件版本后缀（"x3" 或 "x4"），用于匹配 firmware-{suffix}.bin 和 md5-{suffix}.txt
  /// @param outTagName 输出：tag_name 字段值
  /// @param outBody 输出：body 字段值（更新备注）
  /// @param outFirmwareUrl 输出：firmware-{suffix}.bin 的 browser_download_url
  /// @param outMd5Url 输出：md5-{suffix}.txt 的 browser_download_url
  /// @param outUpdateTxtUrl 输出：update.txt 的 browser_download_url（可选，未找到时为空）
  /// @return true 解析成功且找到固件和 MD5 文件
  static bool parseReleaseJson(const std::string& json,
                                const std::string& hardwareSuffix,
                                std::string& outTagName,
                                std::string& outBody,
                                std::string& outFirmwareUrl,
                                std::string& outMd5Url,
                                std::string& outUpdateTxtUrl);

  /// 从 tag_name 字符串中提取整型版本号
  /// 支持格式："20260420"、"v20260420"、"V20260420"
  /// @param tagName tag_name 字符串
  /// @param outVersion 输出：提取的整型版本号
  /// @return true 提取成功
  static bool parseTagVersion(const std::string& tagName, int& outVersion);

  /// 版本对比：remoteVersion > localVersion 时返回 true
  static bool isNewerVersion(int remoteVersion, int localVersion);

 private:
  UpdateInfo updateInfo;

  static constexpr const char* RELEASE_API_URL =
      "https://gitee.com/api/v5/repos/daixinchun/xteink-x3/releases/latest";
  static constexpr const char* SD_UPDATE_DIR = "/update";
  static constexpr const char* SD_FIRMWARE_PATH = "/update/firmware.bin";
  static constexpr const char* SD_MD5_PATH = "/update/md5.txt";
};
