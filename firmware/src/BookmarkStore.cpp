#include "BookmarkStore.h"

#include <HardwareSerial.h>
#include <MD5Builder.h>
#include <SDCardManager.h>
#include <Serialization.h>

#include <algorithm>

std::string BookmarkStore::getBookmarkDir() {
  return "/.crosspoint/bookmarks";
}

std::string BookmarkStore::getBookmarkFilePath(const std::string& bookMd5) {
  return getBookmarkDir() + "/" + bookMd5 + "/bookmarks.bin";
}

std::string BookmarkStore::calculateBookMd5(const std::string& filePath) {
  MD5Builder md5;
  md5.begin();
  md5.add(filePath.c_str());
  md5.calculate();
  return std::string(md5.toString().c_str());
}

std::vector<Bookmark> BookmarkStore::loadBookmarks(const std::string& bookMd5) {
  std::vector<Bookmark> bookmarks;
  const std::string path = getBookmarkFilePath(bookMd5);

  FsFile file;
  if (!SdMan.openFileForRead("BMS", path, file)) {
    return bookmarks;
  }

  uint8_t version;
  serialization::readPod(file, version);
  if (version != FILE_VERSION) {
    Serial.printf("[%lu] [BMS] Version mismatch: expected %d, got %d\n", millis(), FILE_VERSION, version);
    file.close();
    return bookmarks;
  }

  // Read book metadata
  std::string bookPath, bookTitle;
  serialization::readString(file, bookPath);
  serialization::readString(file, bookTitle);

  uint8_t count;
  serialization::readPod(file, count);

  bookmarks.reserve(count);
  for (uint8_t i = 0; i < count; i++) {
    Bookmark bm;
    serialization::readString(file, bm.title);
    serialization::readPod(file, bm.percentage);
    serialization::readPod(file, bm.spineIndex);
    serialization::readPod(file, bm.page);
    serialization::readPod(file, bm.timestamp);
    bm.bookPath = bookPath;
    bm.bookTitle = bookTitle;
    bookmarks.push_back(std::move(bm));
  }

  file.close();
  Serial.printf("[%lu] [BMS] Loaded %d bookmarks for %s\n", millis(), count, bookMd5.c_str());
  return bookmarks;
}

bool BookmarkStore::saveBookmarks(const std::string& bookMd5, const std::vector<Bookmark>& bookmarks,
                                  const std::string& bookPath, const std::string& bookTitle) {
  const std::string dirPath = getBookmarkDir() + "/" + bookMd5;
  SdMan.mkdir(getBookmarkDir().c_str());
  SdMan.mkdir(dirPath.c_str());

  const std::string filePath = getBookmarkFilePath(bookMd5);
  FsFile file;
  if (!SdMan.openFileForWrite("BMS", filePath, file)) {
    Serial.printf("[%lu] [BMS] Failed to open file for write: %s\n", millis(), filePath.c_str());
    return false;
  }

  serialization::writePod(file, FILE_VERSION);

  // Determine book path/title: use provided values, or from first bookmark
  std::string bp = bookPath;
  std::string bt = bookTitle;
  if (bp.empty() && !bookmarks.empty()) {
    bp = bookmarks[0].bookPath;
    bt = bookmarks[0].bookTitle;
  }
  serialization::writeString(file, bp);
  serialization::writeString(file, bt);

  const uint8_t count = static_cast<uint8_t>(std::min(static_cast<size_t>(MAX_BOOKMARKS), bookmarks.size()));
  serialization::writePod(file, count);

  for (uint8_t i = 0; i < count; i++) {
    const auto& bm = bookmarks[i];
    serialization::writeString(file, bm.title);
    serialization::writePod(file, bm.percentage);
    serialization::writePod(file, bm.spineIndex);
    serialization::writePod(file, bm.page);
    serialization::writePod(file, bm.timestamp);
  }

  file.close();
  Serial.printf("[%lu] [BMS] Saved %d bookmarks for %s\n", millis(), count, bookMd5.c_str());
  return true;
}

bool BookmarkStore::addBookmark(const std::string& bookMd5, const Bookmark& bookmark) {
  auto bookmarks = loadBookmarks(bookMd5);

  // Clamp percentage
  Bookmark bm = bookmark;
  if (bm.percentage < 0.0f) bm.percentage = 0.0f;
  if (bm.percentage > 1.0f) bm.percentage = 1.0f;

  // Insert at front (newest first)
  bookmarks.insert(bookmarks.begin(), bm);

  // Trim to max
  if (bookmarks.size() > MAX_BOOKMARKS) {
    bookmarks.resize(MAX_BOOKMARKS);
  }

  return saveBookmarks(bookMd5, bookmarks, bm.bookPath, bm.bookTitle);
}

bool BookmarkStore::deleteBookmark(const std::string& bookMd5, int index) {
  auto bookmarks = loadBookmarks(bookMd5);

  if (index < 0 || index >= static_cast<int>(bookmarks.size())) {
    return false;
  }

  // Preserve book metadata before erasing
  std::string bp = bookmarks[0].bookPath;
  std::string bt = bookmarks[0].bookTitle;

  bookmarks.erase(bookmarks.begin() + index);
  return saveBookmarks(bookMd5, bookmarks, bp, bt);
}

std::vector<Bookmark> BookmarkStore::loadAllBookmarks() {
  std::vector<Bookmark> allBookmarks;
  const std::string baseDir = getBookmarkDir();

  FsFile dir;
  if (!SdMan.openFileForRead("BMS", baseDir, dir)) {
    return allBookmarks;
  }

  if (!dir.isDir()) {
    dir.close();
    return allBookmarks;
  }

  FsFile entry;
  while (entry.openNext(&dir, O_RDONLY)) {
    if (entry.isDir()) {
      char name[64];
      entry.getName(name, sizeof(name));
      entry.close();

      std::string md5Name(name);
      auto bookmarks = loadBookmarks(md5Name);
      for (auto& bm : bookmarks) {
        allBookmarks.push_back(std::move(bm));
      }
    } else {
      entry.close();
    }
  }

  dir.close();

  // Sort all bookmarks by timestamp descending
  std::sort(allBookmarks.begin(), allBookmarks.end(),
            [](const Bookmark& a, const Bookmark& b) { return a.timestamp > b.timestamp; });

  return allBookmarks;
}

bool BookmarkStore::hasAnyBookmarks() {
  const std::string baseDir = getBookmarkDir();

  FsFile dir;
  if (!SdMan.openFileForRead("BMS", baseDir, dir)) {
    return false;
  }

  if (!dir.isDir()) {
    dir.close();
    return false;
  }

  bool found = false;
  FsFile entry;
  while (entry.openNext(&dir, O_RDONLY)) {
    if (entry.isDir()) {
      // Check if this directory has a bookmarks.bin file
      char name[64];
      entry.getName(name, sizeof(name));
      entry.close();

      std::string filePath = baseDir + "/" + std::string(name) + "/bookmarks.bin";
      if (SdMan.exists(filePath.c_str())) {
        found = true;
        break;
      }
    } else {
      entry.close();
    }
  }

  dir.close();
  return found;
}
