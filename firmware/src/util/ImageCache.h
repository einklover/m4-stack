#pragma once

// ─── ImageCache ─────────────────────────────────────────────────────────────
// 图片 .pxc 缓存工具（供 MyLibraryActivity 预览 & SleepActivity 关机壁纸共用）
//
// 缓存目录:  /.crosspoint/lock_screen/
// 缓存文件:  {stem}.pxc    （2-bit packed 像素，与 ImageBlock 格式相同）
// 索引文件:  index.txt      （每行: {source_full_path}|{source_size}|{stem}）
//
// 工作流程:
//   1. 预览图片时调用 getDecodeCachePath() 得到 pxc 路径，作为 renderConfig.cachePath
//   2. 解码成功后调用 commit() 更新索引
//   3. 下次预览/关机同一图片时 isValid() 命中 → renderFromCache() 跳过解码

#include <GfxRenderer.h>
#include <HardwareSerial.h>
#include <SDCardManager.h>
#include <SdFat.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../../lib/Epub/Epub/converters/DitherUtils.h"

namespace ImageCache {

static constexpr const char* CACHE_DIR    = "/.crosspoint/lock_screen";
static constexpr const char* INDEX_FILE   = "/.crosspoint/lock_screen/index.txt";
static constexpr const char* INDEX_HD_FILE = "/.crosspoint/lock_screen/index_hd.txt";

// ── 工具：从路径提取文件 stem（去掉目录和扩展名）────────────────────────────
inline std::string getStem(const std::string& path) {
  size_t slashPos = path.rfind('/');
  std::string name = (slashPos != std::string::npos) ? path.substr(slashPos + 1) : path;
  size_t dotPos = name.rfind('.');
  return (dotPos != std::string::npos) ? name.substr(0, dotPos) : name;
}

// ── 根据源文件路径得到对应的 .pxc 缓存路径 ───────────────────────────────────
inline std::string getPxcPath(const std::string& sourcePath) {
  return std::string(CACHE_DIR) + "/" + getStem(sourcePath) + ".pxc";
}

// ── 根据源文件路径得到 HD(_hd.pxc) 缓存路径 ──────────────────────────────────
inline std::string getHdPxcPath(const std::string& sourcePath) {
  return std::string(CACHE_DIR) + "/" + getStem(sourcePath) + "_hd.pxc";
}

// ── 获取源文件大小（用于缓存校验）────────────────────────────────────────────
inline uint32_t getSourceSize(const std::string& sourcePath) {
  FsFile f;
  if (!SdMan.openFileForRead("ICH", sourcePath, f)) return 0;
  uint32_t sz = static_cast<uint32_t>(f.size());
  f.close();
  return sz;
}

// ── 检查索引中是否有匹配的有效缓存条目 ──────────────────────────────────────
inline bool isValid(const std::string& sourcePath, uint32_t srcSize) {
  if (srcSize == 0) return false;
  if (!SdMan.exists(getPxcPath(sourcePath).c_str())) return false;
  if (!SdMan.exists(INDEX_FILE)) return false;

  FsFile idx;
  if (!SdMan.openFileForRead("ICH", INDEX_FILE, idx)) return false;

  const std::string stem = getStem(sourcePath);
  bool found = false;
  char line[600];

  while (idx.available()) {
    int len = 0;
    int c;
    while (len < (int)sizeof(line) - 1 && (c = idx.read()) != -1 && c != '\n') {
      line[len++] = static_cast<char>(c);
    }
    line[len] = '\0';
    if (len == 0) continue;

    // 格式: sourcePath|sourceSize|stem
    char* p1 = strchr(line, '|');
    if (!p1) continue;
    char* p2 = strchr(p1 + 1, '|');
    if (!p2) continue;

    std::string storedPath(line, p1 - line);
    uint32_t storedSize = static_cast<uint32_t>(atol(p1 + 1));
    std::string storedStem(p2 + 1);

    if (storedPath == sourcePath && storedSize == srcSize && storedStem == stem) {
      found = true;
      break;
    }
  }
  idx.close();
  return found;
}

// ── 检查 HD 缓存是否有效（_hd.pxc + index_hd.txt）────────────────────────────
inline bool isHdValid(const std::string& sourcePath, uint32_t srcSize) {
  if (srcSize == 0) return false;
  if (!SdMan.exists(getHdPxcPath(sourcePath).c_str())) return false;
  if (!SdMan.exists(INDEX_HD_FILE)) return false;

  FsFile idx;
  if (!SdMan.openFileForRead("ICH", INDEX_HD_FILE, idx)) return false;

  const std::string stem = getStem(sourcePath);
  bool found = false;
  char line[600];

  while (idx.available()) {
    int len = 0;
    int c;
    while (len < (int)sizeof(line) - 1 && (c = idx.read()) != -1 && c != '\n') {
      line[len++] = static_cast<char>(c);
    }
    line[len] = '\0';
    if (len == 0) continue;

    char* p1 = strchr(line, '|');
    if (!p1) continue;
    char* p2 = strchr(p1 + 1, '|');
    if (!p2) continue;

    std::string storedPath(line, p1 - line);
    uint32_t storedSize = static_cast<uint32_t>(atol(p1 + 1));
    std::string storedStem(p2 + 1);

    if (storedPath == sourcePath && storedSize == srcSize && storedStem == stem) {
      found = true;
      break;
    }
  }
  idx.close();
  return found;
}

// ── 便捷重载：自动从 SD 卡读取文件大小 ──────────────────────────────────────
inline bool isValid(const std::string& sourcePath) {
  return isValid(sourcePath, getSourceSize(sourcePath));
}

// ── 更新/添加索引条目（sourcePath|sourceSize|stem）──────────────────────────
inline void updateIndex(const std::string& sourcePath, uint32_t srcSize) {
  const std::string stem = getStem(sourcePath);

  // 构建新条目字符串
  char sizeStr[16];
  snprintf(sizeStr, sizeof(sizeStr), "%u", static_cast<unsigned>(srcSize));
  const std::string newEntry = sourcePath + "|" + sizeStr + "|" + stem;

  // 读取现有索引，过滤掉同 stem 的旧条目
  std::vector<std::string> lines;
  FsFile rf;
  if (SdMan.openFileForRead("ICH", INDEX_FILE, rf)) {
    char line[600];
    while (rf.available()) {
      int len = 0;
      int c;
      while (len < (int)sizeof(line) - 1 && (c = rf.read()) != -1 && c != '\n') {
        line[len++] = static_cast<char>(c);
      }
      line[len] = '\0';
      if (len == 0) continue;

      // 如果该行最后一个 | 后面的 stem 相同，跳过（覆盖旧缓存条目）
      char* lastPipe = strrchr(line, '|');
      if (lastPipe && std::string(lastPipe + 1) == stem) continue;
      lines.push_back(std::string(line));
    }
    rf.close();
  }
  lines.push_back(newEntry);

  // 回写索引
  FsFile wf;
  if (SdMan.openFileForWrite("ICH", INDEX_FILE, wf)) {
    for (const auto& l : lines) {
      wf.print(l.c_str());
      wf.print('\n');
    }
    wf.close();
    Serial.printf("[%lu] [ICH] Index updated: %s\n", millis(), newEntry.c_str());
  }
}

// ── 解码完成后提交缓存（写索引）─────────────────────────────────────────────
inline void commit(const std::string& sourcePath, uint32_t srcSize) {
  if (srcSize == 0) return;
  updateIndex(sourcePath, srcSize);
}

// ── HD 缓存提交（写 index_hd.txt）────────────────────────────────────────────
inline void updateHdIndex(const std::string& sourcePath, uint32_t srcSize) {
  const std::string stem = getStem(sourcePath);
  char sizeStr[16];
  snprintf(sizeStr, sizeof(sizeStr), "%u", static_cast<unsigned>(srcSize));
  const std::string newEntry = sourcePath + "|" + sizeStr + "|" + stem;

  std::vector<std::string> lines;
  FsFile rf;
  if (SdMan.openFileForRead("ICH", INDEX_HD_FILE, rf)) {
    char line[600];
    while (rf.available()) {
      int len = 0;
      int c;
      while (len < (int)sizeof(line) - 1 && (c = rf.read()) != -1 && c != '\n') {
        line[len++] = static_cast<char>(c);
      }
      line[len] = '\0';
      if (len == 0) continue;
      char* lastPipe = strrchr(line, '|');
      if (lastPipe && std::string(lastPipe + 1) == stem) continue;
      lines.push_back(std::string(line));
    }
    rf.close();
  }
  lines.push_back(newEntry);
  FsFile wf;
  if (SdMan.openFileForWrite("ICH", INDEX_HD_FILE, wf)) {
    for (const auto& l : lines) { wf.print(l.c_str()); wf.print('\n'); }
    wf.close();
    Serial.printf("[%lu] [ICH] HD index updated: %s\n", millis(), newEntry.c_str());
  }
}

inline void commitHd(const std::string& sourcePath, uint32_t srcSize) {
  if (srcSize == 0) return;
  updateHdIndex(sourcePath, srcSize);
}

// ── 获取 HD 解码时 renderConfig.cachePath（确保目录存在）─────────────────────
inline std::string getHdDecodeCachePath(const std::string& sourcePath) {
  SdMan.mkdir(CACHE_DIR);
  return getHdPxcPath(sourcePath);
}

// ── 从任意 .pxc 文件渲染到 renderer（公共 helper）───────────────────────────
inline bool renderFromPxcFile(const std::string& pxcPath, GfxRenderer& renderer,
                              int x = 0, int y = 0) {
  FsFile f;
  if (!SdMan.openFileForRead("ICH", pxcPath, f)) return false;

  uint16_t w = 0, h = 0;
  if (f.read(&w, 2) != 2 || f.read(&h, 2) != 2) { f.close(); return false; }

  const int bytesPerRow = (w + 3) / 4;
  uint8_t* row = static_cast<uint8_t*>(malloc(bytesPerRow));
  if (!row) { f.close(); return false; }

  for (int py = 0; py < h; py++) {
    if (f.read(row, bytesPerRow) != bytesPerRow) { free(row); f.close(); return false; }
    for (int px = 0; px < w; px++) {
      uint8_t pv = (row[px / 4] >> (6 - (px % 4) * 2)) & 0x03;
      drawPixelWithRenderMode(renderer, x + px, y + py, pv);
    }
  }
  free(row);
  f.close();
  Serial.printf("[%lu] [ICH] Rendered pxc: %s (%dx%d)\n", millis(), pxcPath.c_str(), w, h);
  return true;
}

// ── 获取解码时应传入 renderConfig.cachePath 的路径（同时确保目录存在）────────
inline std::string getDecodeCachePath(const std::string& sourcePath) {
  SdMan.mkdir(CACHE_DIR);
  return getPxcPath(sourcePath);
}

// ── 从 .pxc 缓存渲染到 renderer（x=0, y=0 为屏幕左上角）────────────────────
inline bool renderFromCache(const std::string& sourcePath, GfxRenderer& renderer,
                            int x = 0, int y = 0) {
  const std::string pxc = getPxcPath(sourcePath);
  FsFile f;
  if (!SdMan.openFileForRead("ICH", pxc, f)) return false;

  uint16_t w = 0, h = 0;
  if (f.read(&w, 2) != 2 || f.read(&h, 2) != 2) {
    f.close();
    return false;
  }

  const int bytesPerRow = (w + 3) / 4;
  uint8_t* row = static_cast<uint8_t*>(malloc(bytesPerRow));
  if (!row) {
    f.close();
    return false;
  }

  for (int py = 0; py < h; py++) {
    if (f.read(row, bytesPerRow) != bytesPerRow) {
      free(row);
      f.close();
      return false;
    }
    for (int px = 0; px < w; px++) {
      uint8_t pv = (row[px / 4] >> (6 - (px % 4) * 2)) & 0x03;
      drawPixelWithRenderMode(renderer, x + px, y + py, pv);
    }
  }

  free(row);
  f.close();
  Serial.printf("[%lu] [ICH] Rendered from cache: %s (%dx%d)\n", millis(), pxc.c_str(), w, h);
  return true;
}

// ── 从 HD 缓存（_hd.pxc）渲染到 renderer ────────────────────────────────────
inline bool renderFromHdCache(const std::string& sourcePath, GfxRenderer& renderer,
                              int x = 0, int y = 0) {
  return renderFromPxcFile(getHdPxcPath(sourcePath), renderer, x, y);
}

}  // namespace ImageCache
