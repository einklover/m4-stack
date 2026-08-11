/**
 * XtcParser.cpp
 *
 * XTC file parsing implementation
 * XTC ebook support for CrossPoint Reader
 */

#include "XtcParser.h"

#include <FsHelpers.h>
#include <HardwareSerial.h>
#include <SDCardManager.h>

#include <cstring>
#include <new>  // for std::bad_alloc
#include "esp_heap_caps.h" //for test最大内存

namespace xtc {

XtcParser::XtcParser()
    : m_isOpen(false),
      m_defaultWidth(DISPLAY_WIDTH),
      m_defaultHeight(DISPLAY_HEIGHT),
      m_bitDepth(1),
      m_hasChapters(false),
      m_lastError(XtcError::OK),
      m_loadBatchSize(200),  // 减小
      m_loadedMaxPage(0),
      m_loadedStartPage(0) {  // 记录当前页表的起始页 
  memset(&m_header, 0, sizeof(m_header));
}

XtcParser::~XtcParser() { 
    if (m_tempBuffer) {
    free(m_tempBuffer);
    m_tempBuffer = nullptr;
    m_tempBufferSize = 0;
  }
  close(); }

XtcError XtcParser::open(const char* filepath) {
  // Close if already open
  if (m_isOpen) {
    close();
  }

  // Open file
  if (!SdMan.openFileForRead("XTC", filepath, m_file)) {
    m_lastError = XtcError::FILE_NOT_FOUND;
    return m_lastError;
  }

  // Read header
  m_lastError = readHeader();
  if (m_lastError != XtcError::OK) {
    Serial.printf("[%lu] [XTC] Failed to read header: %s\n", millis(), errorToString(m_lastError));
    m_file.close();
    return m_lastError;
  }

  // Read title & author if available
  if (m_header.hasMetadata) {
    m_lastError = readTitle();
    if (m_lastError != XtcError::OK) {
      Serial.printf("[%lu] [XTC] Failed to read title: %s\n", millis(), errorToString(m_lastError));
      m_file.close();
      return m_lastError;
    }
    m_lastError = readAuthor();
    if (m_lastError != XtcError::OK) {
      Serial.printf("[%lu] [XTC] Failed to read author: %s\n", millis(), errorToString(m_lastError));
      m_file.close();
      return m_lastError;
    }
  }

  // Read page table
  m_lastError = readPageTable();
  if (m_lastError != XtcError::OK) {
    Serial.printf("[%lu] [XTC] Failed to read page table: %s\n", millis(), errorToString(m_lastError));
    m_file.close();
    return m_lastError;
  }

  // ✅ 恢复原有章节读取逻辑（不调用readChapters_gd）
  m_lastError = readChapters();
  if (m_lastError != XtcError::OK) {
    Serial.printf("[%lu] [XTC] Failed to read internal chapters: %s\n", millis(), errorToString(m_lastError));
    // 读取失败时创建默认章节，不退出
    m_chapters.clear();
    std::string chapterName = m_title.empty() ? "全书" : m_title;
    ChapterInfo singleChapter{std::move(chapterName), 0, m_header.pageCount - 1};
    m_chapters.push_back(std::move(singleChapter));
    m_hasChapters = true;
  }

  m_isOpen = true;
  Serial.printf("[%lu] [XTC] Opened file: %s (%u pages, %dx%d, %u internal chapters)\n", millis(), filepath, 
                m_header.pageCount, m_defaultWidth, m_defaultHeight, (uint16_t)m_chapters.size());
  return XtcError::OK;
}

void XtcParser::close() {
  if (m_isOpen) {
    m_file.close();
    m_isOpen = false;
  }
  m_pageTable.clear();
  m_chapters.clear();
  m_title.clear();
  m_hasChapters = false;
  m_loadedMaxPage = 0;
  m_loadedStartPage = 0;
  m_sliceCacheValid = false;
  m_sliceCachePageIndex = 0;
  m_sliceCachePageOffset = 0;
  memset(&m_sliceCacheHeader, 0, sizeof(m_sliceCacheHeader));
  memset(&m_header, 0, sizeof(m_header));
  
  // ✅ 仅清空独立的GD章节数据，不影响内部状态
  chapterActualCount = 0;
  memset(ChapterList, 0, sizeof(ChapterList));
}

XtcError XtcParser::readHeader() {
  // Read first 56 bytes of header
  size_t bytesRead = m_file.read(reinterpret_cast<uint8_t*>(&m_header), sizeof(XtcHeader));
  if (bytesRead != sizeof(XtcHeader)) {
    return XtcError::READ_ERROR;
  }

  // Verify magic number (accept both XTC and XTCH)
  if (m_header.magic != XTC_MAGIC && m_header.magic != XTCH_MAGIC) {
    Serial.printf("[%lu] [XTC] Invalid magic: 0x%08X (expected 0x%08X or 0x%08X)\n", millis(), m_header.magic,
                  XTC_MAGIC, XTCH_MAGIC);
    return XtcError::INVALID_MAGIC;
  }

  // Determine bit depth from file magic
  m_bitDepth = (m_header.magic == XTCH_MAGIC) ? 2 : 1;

  // Check version
  const bool validVersion = m_header.versionMajor == 1 && m_header.versionMinor == 0 ||
                            m_header.versionMajor == 0 && m_header.versionMinor == 1;
  if (!validVersion) {
    Serial.printf("[%lu] [XTC] Unsupported version: %u.%u\n", millis(), m_header.versionMajor, m_header.versionMinor);
    return XtcError::INVALID_VERSION;
  }

  // Basic validation
  if (m_header.pageCount == 0) {
    return XtcError::CORRUPTED_HEADER;
  }

  Serial.printf("[%lu] [XTC] Header: magic=0x%08X (%s), ver=%u.%u, pages=%u, bitDepth=%u\n", millis(), m_header.magic,
                (m_header.magic == XTCH_MAGIC) ? "XTCH" : "XTC", m_header.versionMajor, m_header.versionMinor,
                m_header.pageCount, m_bitDepth);

  return XtcError::OK;
}

XtcError XtcParser::readTitle() {
  constexpr auto titleOffset = 0x38;
  if (!m_file.seek(titleOffset)) {
    return XtcError::READ_ERROR;
  }

  char titleBuf[128] = {0};
  m_file.read(titleBuf, sizeof(titleBuf) - 1);
  m_title = titleBuf;

  Serial.printf("[%lu] [XTC] Title: %s\n", millis(), m_title.c_str());
  return XtcError::OK;
}

XtcError XtcParser::readAuthor() {
  constexpr auto authorOffset = 0xB8;
  if (!m_file.seek(authorOffset)) {
    return XtcError::READ_ERROR;
  }

  char authorBuf[64] = {0};
  m_file.read(authorBuf, sizeof(authorBuf) - 1);
  m_author = authorBuf;

  Serial.printf("[%lu] [XTC] Author: %s\n", millis(), m_author.c_str());
  return XtcError::OK;
}

XtcError XtcParser::readPageTable() {
  m_pageTable.clear();          
  m_pageTable.shrink_to_fit();  
  if (m_header.pageTableOffset == 0) {
    Serial.printf("[%lu] [XTC] Page table offset is 0, cannot read\n", millis());
    return XtcError::CORRUPTED_HEADER;
  }

  // Seek to page table
  if (!m_file.seek(m_header.pageTableOffset)) {
    Serial.printf("[%lu] [XTC] Failed to seek to page table at %llu\n", millis(), m_header.pageTableOffset);
    return XtcError::READ_ERROR;
  }
  
  // 初始加载：从第0页开始，加载第一批2000页
  uint16_t startPage = 0;
  uint16_t endPage = startPage + m_loadBatchSize - 1;
  if(endPage >= m_header.pageCount) endPage = m_header.pageCount - 1;
  uint16_t loadCount = endPage - startPage + 1;

  m_pageTable.resize(endPage + 1);

  // Read page table entries
  for (uint16_t i = startPage; i <= endPage; i++) {
    PageTableEntry entry;
    size_t bytesRead = m_file.read(reinterpret_cast<uint8_t*>(&entry), sizeof(PageTableEntry));
    if (bytesRead != sizeof(PageTableEntry)) {
      Serial.printf("[%lu] [XTC] Failed to read page table entry %u\n", millis(), i);
      return XtcError::READ_ERROR;
    }

    m_pageTable[i].offset = static_cast<uint32_t>(entry.dataOffset);
    m_pageTable[i].size = entry.dataSize;
    m_pageTable[i].width = entry.width;
    m_pageTable[i].height = entry.height;
    m_pageTable[i].bitDepth = m_bitDepth;

    // Update default dimensions from first page
    if (i == 0) {
      m_defaultWidth = entry.width;
      m_defaultHeight = entry.height;
    }
  }
  m_loadedMaxPage = endPage;
  Serial.printf("[%lu] [XTC] Read %u page table entries (batch 0~%u)\n", millis(), loadCount, endPage);
  return XtcError::OK;
}

// ============================================================
// readChapters: 开文件时一次性将所有章节加载到 m_chapters
// 章节条目格式：96字节/章
//   [0-79]  : 章节名 (UTF-8 字符串)
//   [80-81] : 起始页码 (uint16_t, 1-based)
//   [82-95] : 保留
// ============================================================
XtcError XtcParser::readChapters() {
  m_hasChapters = false;
  m_chapters.clear();
  maxChapterCount = 0;

  constexpr size_t CHAPTER_ENTRY_SIZE = 96;

  // 检查文件头章节标记和偏移
  if (!m_header.hasChapters || m_header.chapterOffset == 0) {
    goto make_default;
  }

  {
    const uint64_t fileSize = m_file.size();

    // 章节区域在 chapterOffset 到 pageTableOffset 之间
    // 如果 pageTableOffset 在 chapterOffset 后面，用它作为章节区的结束
    uint64_t maxOffset = (m_header.pageTableOffset > m_header.chapterOffset)
                            ? m_header.pageTableOffset
                            : fileSize;
    uint64_t available = (maxOffset > m_header.chapterOffset)
                            ? (maxOffset - m_header.chapterOffset)
                            : 0;
    size_t totalChapters = (size_t)(available / CHAPTER_ENTRY_SIZE);

    Serial.printf("[%lu] [XTC] 章节区: offset=%u, available=%llu, 共%u章\n",
                 millis(), m_header.chapterOffset, available, (unsigned)totalChapters);

    if (totalChapters == 0) goto make_default;

    if (!m_file.seek(m_header.chapterOffset)) goto make_default;

    uint8_t buf[CHAPTER_ENTRY_SIZE];
    for (size_t i = 0; i < totalChapters; i++) {
      if (m_file.read(buf, CHAPTER_ENTRY_SIZE) != CHAPTER_ENTRY_SIZE) break;

      // 章节名前 80 字节，检查是否为空
      char nameBuf[81] = {0};
      memcpy(nameBuf, buf, 80);
      bool isEmpty = true;
      for (int j = 0; j < 80; j++) {
        if (nameBuf[j] != 0) { isEmpty = false; break; }
      }
      if (isEmpty) continue;

      // 起始页码：偏移 80，2字节，1-based
      uint16_t startPage = 0;
      memcpy(&startPage, buf + 80, sizeof(uint16_t));
      if (startPage > 0) startPage -= 1;  // 转为 0-based

      ChapterInfo chapter;
      chapter.name = nameBuf;
      chapter.startPage = startPage;
      chapter.endPage = 0;  // 暂时不填
      m_chapters.push_back(std::move(chapter));

      Serial.printf("[%lu] [XTC] 章节[%u]: [%s] 起始页=%u\n",
                   millis(), (unsigned)m_chapters.size() - 1,
                   m_chapters.back().name.c_str(), m_chapters.back().startPage);
    }

    // 填充每章 endPage
    for (size_t i = 0; i + 1 < m_chapters.size(); i++) {
      m_chapters[i].endPage = m_chapters[i + 1].startPage - 1;
    }
    if (!m_chapters.empty()) {
      m_chapters.back().endPage = m_header.pageCount - 1;
    }
  }

  if (m_chapters.empty()) goto make_default;

  m_hasChapters = true;
  maxChapterCount = m_chapters.size();
  Serial.printf("[%lu] [XTC] 共读到 %u 个章节\n", millis(), (unsigned)m_chapters.size());
  return XtcError::OK;

make_default:
  {
    std::string chapterName = m_title.empty() ? "全书" : m_title;
    ChapterInfo singleChapter{std::move(chapterName), 0,
                              static_cast<uint16_t>(m_header.pageCount - 1)};
    m_chapters.push_back(std::move(singleChapter));
    m_hasChapters = true;
    maxChapterCount = 1;
    Serial.printf("[%lu] [XTC] 创建默认章节\n", millis());
    return XtcError::OK;
  }
}

// readChapters_gd: 直接从已加载的 m_chapters 填充 ChapterList，不再读文件
XtcError XtcParser::readChapters_gd(uint16_t chapterStart) {
    chapterActualCount = 0;
    memset(ChapterList, 0, sizeof(ChapterList));

    if (m_chapters.empty() || chapterStart >= (uint16_t)m_chapters.size()) {
        return XtcError::OK;
    }

    size_t endIdx = std::min((size_t)chapterStart + MAX_SAVE_CHAPTER, m_chapters.size());
    for (size_t i = chapterStart; i < endIdx; i++) {
        int slot = (int)(i - chapterStart);
        ChapterList[slot].chapterIndex = (int)i;
        strncpy(ChapterList[slot].shortTitle, m_chapters[i].name.c_str(), TITLE_BUF_SIZE - 1);
        ChapterList[slot].shortTitle[TITLE_BUF_SIZE - 1] = '\0';
        ChapterList[slot].startPage = m_chapters[i].startPage;
        chapterActualCount++;
    }
    return XtcError::OK;
}
    
// ========== 动态加载相关函数 ==========
XtcError XtcParser::loadNextPageBatch() {
  if(!m_isOpen) return XtcError::FILE_NOT_FOUND;
  if(m_loadedMaxPage >= m_header.pageCount - 1) {
    Serial.printf("[XTC] 已加载全部%u页\n", m_header.pageCount);
    return XtcError::PAGE_OUT_OF_RANGE;
  }
  return loadPageBatchByStart(m_loadedMaxPage + 1);
}

uint16_t XtcParser::getLoadedMaxPage() const {
  return m_loadedMaxPage;
}

uint16_t XtcParser::getPageBatchSize() const {
  return m_loadBatchSize;
}

// ✅ 修复getPageInfo中的索引计算错误
bool XtcParser::getPageInfo(uint32_t pageIndex, PageInfo& info) const {
  if (pageIndex >= m_header.pageCount) return false;
  
  // 检查是否需要加载新批次
  if (pageIndex < m_loadedStartPage || pageIndex > m_loadedMaxPage) {
    auto* self = const_cast<XtcParser*>(this);
    // 修复：直接加载目标页所在的批次，而非按batchSize取整
    self->loadPageBatchByStart((uint16_t)pageIndex); 
  }
  
  // 计算当前页在页表中的相对索引
  uint16_t idx = (uint16_t)(pageIndex - m_loadedStartPage);
  if(idx >= m_pageTable.size()) return false;
  
  info = m_pageTable[idx];
  return true;
}


// 内联错误码转字符串函数，提升日志可读性


size_t XtcParser::loadPage(uint32_t pageIndex, uint8_t* buffer, size_t bufferSize, 
                          bool isSliceMode, int n ) {
    // ========== 核心控制参数 ==========
    const int SLICE_COUNT = 8;       // 固定8等分（核心，保证显示正确）

    // ========== 基础防护1：核心参数合法性 ==========
    if (!m_isOpen || pageIndex >= m_header.pageCount || buffer == nullptr) {
        Serial.printf("[%lu] [XTC] Invalid param: open=%d, page=%lu/%lu, buffer=%p\n",
                      millis(), m_isOpen, pageIndex, m_header.pageCount, buffer);
        // 内存日志
        size_t freeMem = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t maxBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        Serial.printf("[MEM] Free: %lu KB, Max block: %lu KB\n", freeMem/1024, maxBlock/1024);
        return 0;
    }

    // ========== 加载页面批次（原有逻辑） ==========
    if (pageIndex < m_loadedStartPage || pageIndex > m_loadedMaxPage) {
        loadPageBatchByStart((uint16_t)pageIndex); // 保持不变
    }

    // ========== 基础防护2：页面索引合法性 ==========
    uint16_t idx = (uint16_t)(pageIndex - m_loadedStartPage);
    if (idx >= m_pageTable.size()) {
        Serial.printf("[%lu] [XTC] Page index out of range: idx=%u, table size=%u\n",
                      millis(), idx, m_pageTable.size());
        // 内存日志
        size_t freeMem = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t maxBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        Serial.printf("[MEM] Free: %lu KB, Max block: %lu KB\n", freeMem/1024, maxBlock/1024);
        return 0;
    }

    // ========== 定位页面信息 ==========
    const PageInfo& page = m_pageTable[idx];

    if (isSliceMode && (n < 0 || n >= SLICE_COUNT)) {
      Serial.printf("[%lu] [XTC] Invalid slice index: %d\n", millis(), n);
      m_lastError = XtcError::PAGE_OUT_OF_RANGE;
      return 0;
    }

    XtgPageHeader pageHeader;
    const uint32_t expectedMagic = (m_bitDepth == 2) ? XTH_MAGIC : XTG_MAGIC;

    if (isSliceMode && m_sliceCacheValid && m_sliceCachePageIndex == pageIndex && m_sliceCachePageOffset == page.offset) {
      pageHeader = m_sliceCacheHeader;
    } else {
      if (!m_file.seek(page.offset)) {
        Serial.printf("[%lu] [XTC] Seek failed: page=%lu, offset=%lu (Load error)\n", millis(), pageIndex, page.offset);
        size_t freeMem = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t maxBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        Serial.printf("[MEM] Free: %lu KB, Max block: %lu KB\n", freeMem/1024, maxBlock/1024);
        return 0;
      }

      size_t headerRead = m_file.read(reinterpret_cast<uint8_t*>(&pageHeader), sizeof(XtgPageHeader));
      if (headerRead != sizeof(XtgPageHeader)) {
        Serial.printf("[%lu] [XTC] Read header failed: page=%lu, read=%lu/%lu (Load error)\n",
                millis(), pageIndex, headerRead, sizeof(XtgPageHeader));
        size_t freeMem = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t maxBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        Serial.printf("[MEM] Free: %lu KB, Max block: %lu KB\n", freeMem/1024, maxBlock/1024);
        return 0;
      }

      if (pageHeader.magic != expectedMagic) {
        Serial.printf("[%lu] [XTC] Invalid magic: page=%lu, got=0x%08X, expect=0x%08X (Load error)\n",
                millis(), pageIndex, pageHeader.magic, expectedMagic);
        size_t freeMem = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t maxBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        Serial.printf("[MEM] Free: %lu KB, Max block: %lu KB\n", freeMem/1024, maxBlock/1024);
        return 0;
      }

      if (isSliceMode) {
        m_sliceCacheValid = true;
        m_sliceCachePageIndex = pageIndex;
        m_sliceCachePageOffset = page.offset;
        m_sliceCacheHeader = pageHeader;
      }
    }

    // ========== 计算完整数据大小 ==========
    size_t bitmapSize;
    if (m_bitDepth == 2) {
        bitmapSize = ((static_cast<size_t>(pageHeader.width) * pageHeader.height + 7) / 8) * 2;
    } else {
      bitmapSize = ((static_cast<size_t>(pageHeader.width) + 7) / 8) * pageHeader.height;
    }

    // ========== 模式1：完整页面加载（默认） ==========
    if (!isSliceMode) {
        if (bufferSize < bitmapSize) {
            Serial.printf("[%lu] [XTC] Full buffer too small: need=%lu, have=%lu (Load error)\n",
                          millis(), bitmapSize, bufferSize);
            // 内存日志
            size_t freeMem = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            size_t maxBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
            Serial.printf("[MEM] Free: %lu KB, Max block: %lu KB (Need: %lu KB)\n", 
                          freeMem/1024, maxBlock/1024, bitmapSize/1024);
            return 0;
        }

        size_t bytesRead = m_file.read(buffer, bitmapSize);
        if (bytesRead != bitmapSize) {
            Serial.printf("[%lu] [XTC] Read full page failed: page=%lu, read=%lu/%lu (Load error)\n",
                          millis(), pageIndex, bytesRead, bitmapSize);
            // 内存日志
            size_t freeMem = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            size_t maxBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
            Serial.printf("[MEM] Free: %lu KB, Max block: %lu KB\n", freeMem/1024, maxBlock/1024);
            return 0;
        }

        Serial.printf("[%lu] [XTC] Load full page success: page=%lu, size=%lu\n", millis(), pageIndex, bytesRead);
        return bytesRead;
    }



// XTCH 2bit 竖切版

size_t sliceBufferSize;


// ------------ 核心：按XTCH垂直列优先计算切片大小 ------------
const uint16_t sliceWidth = pageHeader.width / SLICE_COUNT; // 按宽度竖切（比如480/8=60列/片）
const uint16_t sliceHeight = pageHeader.height / SLICE_COUNT; // 按高度竖切（比如800/8=100行/片）
size_t slicePlaneSize;
if (m_bitDepth == 2) {
    // XTCH 2bit：单个平面 = 切片宽度 × ((高度 +7)/8)（竖切列数 × 每列字节数）
    slicePlaneSize = sliceWidth * ((pageHeader.height + 7) / 8);
    sliceBufferSize = slicePlaneSize * 2; // 双平面
} else {
    // 1bit 仍按行切（不影响）
  slicePlaneSize = ((static_cast<size_t>(pageHeader.width) + 7) / 8) * sliceHeight;
    sliceBufferSize = slicePlaneSize;
}

// 缓冲区校验
if (bufferSize < sliceBufferSize) {
  //debug
  Serial.printf("[%lu] [XTC] 查看片高:%d,片宽:%d\n", millis(), sliceHeight, pageHeader.width);
    Serial.printf("[%lu] [XTC] Buffer too small: need %lu, have %lu\n", millis(), sliceBufferSize, bufferSize);
    m_lastError = XtcError::MEMORY_ERROR;
    return 0;
}
memset(buffer, 0, sliceBufferSize);

if (m_bitDepth == 2) {
    // XTCH 2bit 竖切核心逻辑
    const size_t fullColBytes = (pageHeader.height + 7) / 8;    // 每列字节数（垂直8像素打包）
    const size_t fullPlaneSize = pageHeader.width * fullColBytes; // 单个平面总大小
  const uint32_t dataStartOffset = page.offset + sizeof(XtgPageHeader);

  // ===== 步骤1：读取平面1的第n个竖切片 =====
  const size_t skipPlane1 = static_cast<size_t>(n) * sliceWidth * fullColBytes;
  const uint32_t plane1Offset = dataStartOffset + static_cast<uint32_t>(skipPlane1);
  if (!m_file.seek(plane1Offset)) {
    Serial.printf("[%lu] [XTC] Plane1 seek failed: offset=%lu\n", millis(), plane1Offset);
    m_lastError = XtcError::READ_ERROR;
    return 0;
  }
    // 读取平面1竖切片（直接到buffer前半段）
    size_t plane1Read = m_file.read(buffer, slicePlaneSize);
    if (plane1Read != slicePlaneSize) {
        Serial.printf("[%lu] [XTC] Plane1 slice read error: expected %lu, got %lu\n", millis(), slicePlaneSize, plane1Read);
        m_lastError = XtcError::READ_ERROR;
        return 0;
    }

    // ===== 步骤2：读取平面2的第n个竖切片 =====
  const uint32_t plane2Offset = dataStartOffset + static_cast<uint32_t>(fullPlaneSize + skipPlane1);
  if (!m_file.seek(plane2Offset)) {
    Serial.printf("[%lu] [XTC] Plane2 seek failed: offset=%lu\n", millis(), plane2Offset);
    m_lastError = XtcError::READ_ERROR;
    return 0;
  }
    // 读取平面2竖切片（直接到buffer后半段）
    size_t plane2Read = m_file.read(buffer + slicePlaneSize, slicePlaneSize);
    if (plane2Read != slicePlaneSize) {
        Serial.printf("[%lu] [XTC] Plane2 slice read error: expected %lu, got %lu\n", millis(), slicePlaneSize, plane2Read);
        m_lastError = XtcError::READ_ERROR;
        return 0;
    }
} else {
    // 1bit 水平切(横着存的)
    const size_t rowBytes = (pageHeader.width + 7) / 8;
  const size_t skipRows = static_cast<size_t>(n) * sliceHeight;
  const uint32_t dataStartOffset = page.offset + sizeof(XtgPageHeader);
  const uint32_t sliceOffset = dataStartOffset + static_cast<uint32_t>(skipRows * rowBytes);
  if (!m_file.seek(sliceOffset)) {
    Serial.printf("[%lu] [XTC] 1bit slice seek failed: offset=%lu\n", millis(), sliceOffset);
    m_lastError = XtcError::READ_ERROR;
    return 0;
    }
    size_t sliceRead = m_file.read(buffer, sliceBufferSize);
    if (sliceRead != sliceBufferSize) {
        Serial.printf("[%lu] [XTC] 1bit slice read error: expected %lu, got %lu\n", millis(), sliceBufferSize, sliceRead);
        m_lastError = XtcError::READ_ERROR;
        return 0;
    }
}

m_lastError = XtcError::OK;
return sliceBufferSize;
};

XtcError XtcParser::loadPageStreaming(uint32_t pageIndex,
                                      std::function<void(const uint8_t* data, size_t size, size_t offset)> callback,
                                      size_t chunkSize) {
  if (!m_isOpen) {
    return XtcError::FILE_NOT_FOUND;
  }

  if (pageIndex >= m_header.pageCount) {
    return XtcError::PAGE_OUT_OF_RANGE;
  }

  // 确保页表已加载
  if (pageIndex < m_loadedStartPage || pageIndex > m_loadedMaxPage) {
    loadPageBatchByStart((uint16_t)pageIndex);
  }
  uint16_t idx = (uint16_t)(pageIndex - m_loadedStartPage);
  if (idx >= m_pageTable.size()) {
    return XtcError::PAGE_OUT_OF_RANGE;
  }
  
  const PageInfo& page = m_pageTable[idx];

  if (!m_file.seek(page.offset)) {
    return XtcError::READ_ERROR;
  }

  XtgPageHeader pageHeader;
  size_t headerRead = m_file.read(reinterpret_cast<uint8_t*>(&pageHeader), sizeof(XtgPageHeader));
  const uint32_t expectedMagic = (m_bitDepth == 2) ? XTH_MAGIC : XTG_MAGIC;
  if (headerRead != sizeof(XtgPageHeader) || pageHeader.magic != expectedMagic) {
    return XtcError::READ_ERROR;
  }

  size_t bitmapSize;
  if (m_bitDepth == 2) {
    bitmapSize = ((static_cast<size_t>(pageHeader.width) * pageHeader.height + 7) / 8) * 2;
  } else {
    bitmapSize = ((pageHeader.width + 7) / 8) * pageHeader.height;
  }

  std::vector<uint8_t> chunk(chunkSize);
  size_t totalRead = 0;

  while (totalRead < bitmapSize) {
    size_t toRead = std::min(chunkSize, bitmapSize - totalRead);
    size_t bytesRead = m_file.read(chunk.data(), toRead);

    if (bytesRead == 0) {
      return XtcError::READ_ERROR;
    }

    callback(chunk.data(), bytesRead, totalRead);
    totalRead += bytesRead;
  }

  return XtcError::OK;
}

bool XtcParser::isValidXtcFile(const char* filepath) {
  FsFile file;
  if (!SdMan.openFileForRead("XTC", filepath, file)) {
    return false;
  }

  uint32_t magic = 0;
  size_t bytesRead = file.read(reinterpret_cast<uint8_t*>(&magic), sizeof(magic));
  file.close();

  if (bytesRead != sizeof(magic)) {
    return false;
  }

  return (magic == XTC_MAGIC || magic == XTCH_MAGIC);
}

// XtcParser.cpp
XtcError XtcParser::loadPageBatchByStart(uint16_t startPage) {
    if(!m_isOpen) {
        Serial.printf("[%lu] [XTC] 加载批次失败：文件未打开\n", millis());
        return XtcError::FILE_NOT_FOUND;
    }
    if(startPage >= m_header.pageCount) {
        Serial.printf("[%lu] [XTC] 加载批次失败：起始页%u超出总页数%u\n", millis(), startPage, m_header.pageCount);
        return XtcError::PAGE_OUT_OF_RANGE;
    }

    // 保存当前状态（加载失败时回滚）
    uint16_t oldStart = m_loadedStartPage;
    uint16_t oldMax = m_loadedMaxPage;
    std::vector<PageInfo> oldPageTable = m_pageTable;

    // 计算批次范围（严格边界校验）
    m_loadedStartPage = startPage;
    uint16_t endPage = startPage + m_loadBatchSize - 1;
    endPage = (endPage >= m_header.pageCount) ? (m_header.pageCount - 1) : endPage;
    uint16_t loadCount = endPage - startPage + 1;

    // 定位页表位置（添加64位偏移校验）
    uint64_t seekOffset = m_header.pageTableOffset + (static_cast<uint64_t>(startPage) * sizeof(PageTableEntry));
    if (seekOffset >= m_file.size()) {
        Serial.printf("[%lu] [XTC] 加载批次失败：偏移%llu超出文件大小%llu\n", millis(), seekOffset, m_file.size());
        // 回滚状态
        m_loadedStartPage = oldStart;
        m_loadedMaxPage = oldMax;
        m_pageTable = oldPageTable;
        return XtcError::READ_ERROR;
    }

    if(!m_file.seek(seekOffset)) {
        Serial.printf("[%lu] [XTC] 加载批次失败：无法定位到偏移%llu\n", millis(), seekOffset);
        // 回滚状态
        m_loadedStartPage = oldStart;
        m_loadedMaxPage = oldMax;
        m_pageTable = oldPageTable;
        return XtcError::READ_ERROR;
    }

    // 加载页表（逐行校验，失败则回滚）
    m_pageTable.resize(loadCount);
    bool loadSuccess = true;
    for(uint16_t i = startPage; i <= endPage; i++) {
        PageTableEntry entry;
        if(m_file.read(reinterpret_cast<uint8_t*>(&entry), sizeof(PageTableEntry)) != sizeof(PageTableEntry)) {
            Serial.printf("[%lu] [XTC] 加载批次失败：读取第%u页表项失败\n", millis(), i);
            loadSuccess = false;
            break;
        }
        uint16_t relIdx = i - startPage;
        m_pageTable[relIdx].offset = static_cast<uint32_t>(entry.dataOffset);
        m_pageTable[relIdx].size = entry.dataSize;
        m_pageTable[relIdx].width = entry.width;
        m_pageTable[relIdx].height = entry.height;
        m_pageTable[relIdx].bitDepth = m_bitDepth;

        // 校验页表项有效性（避免无效偏移）
        if (m_pageTable[relIdx].offset >= m_file.size()) {
            Serial.printf("[%lu] [XTC] 加载批次失败：第%u页偏移%lu超出文件大小\n", millis(), i, m_pageTable[relIdx].offset);
            loadSuccess = false;
            break;
        }
    }

    if (!loadSuccess) {
        // 回滚到旧状态
        m_loadedStartPage = oldStart;
        m_loadedMaxPage = oldMax;
        m_pageTable = oldPageTable;
        return XtcError::READ_ERROR;
    }

    m_loadedMaxPage = endPage;
    Serial.printf("[%lu] [XTC] 加载批次 ✔️ : [%u~%u] (共%u页) | 页表项数=%u\n", 
                 millis(), startPage, endPage, loadCount, (uint16_t)m_pageTable.size());
    return XtcError::OK;
}

// ========== 新增：释放批次内存（解决内存泄漏核心） ==========
// XtcParser.cpp
void XtcParser::releasePageBatchByStart(uint16_t startPage) {
    if (!m_isOpen) {
        Serial.printf("[%lu] [XTC] 释放批次失败：文件未打开\n", millis());
        return;
    }

    // 仅当起始页匹配当前加载批次时才释放（避免误释放）
    if (startPage == m_loadedStartPage && m_loadedMaxPage >= m_loadedStartPage) {
        // 清空页表但保留vector容器（避免后续resize时重新分配内存导致碎片）
        m_pageTable.clear(); 
        // 重置状态为"无有效批次"（关键：后续加载会检测此状态）
        m_loadedStartPage = 0;
        m_loadedMaxPage = 0;
        
        Serial.printf("[%lu] [XTC] 释放批次 ✔️ : [%u~%u] | 页表已清空\n", 
                     millis(), startPage, m_loadedMaxPage);
    } else {
        Serial.printf("[%lu] [XTC] 跳过释放：批次[%u]非当前加载批次[%u~%u]\n", 
                     millis(), startPage, m_loadedStartPage, m_loadedMaxPage);
    }
}

// getChapterIndexByPage: 线性搜索 m_chapters，返回指定页所属章节索引
uint16_t XtcParser::getChapterIndexByPage(uint16_t pageNum) {
    if (m_chapters.empty()) return 0;

    uint16_t result = 0;
    for (size_t i = 0; i < m_chapters.size(); i++) {
        if (m_chapters[i].startPage <= pageNum) {
            result = (uint16_t)i;
        } else {
            break;
        }
    }
    return result;
}

}  // namespace xtc