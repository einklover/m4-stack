#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Bookmark {
  std::string title;       // "前5字...（第X页, XX%）"
  float percentage;        // 0.0-1.0 归一化百分比
  int spineIndex;
  int page;
  int64_t timestamp;       // Unix 时间戳（毫秒）
  std::string bookPath;    // 书籍文件路径
  std::string bookTitle;   // 书籍标题
};

class BookmarkStore {
 public:
  // 加载指定书籍的所有书签（按时间倒序）
  static std::vector<Bookmark> loadBookmarks(const std::string& bookMd5);

  // 保存完整书签列表到文件
  static bool saveBookmarks(const std::string& bookMd5, const std::vector<Bookmark>& bookmarks,
                            const std::string& bookPath = "", const std::string& bookTitle = "");

  // 添加一条书签并保存
  static bool addBookmark(const std::string& bookMd5, const Bookmark& bookmark);

  // 删除指定索引的书签并保存
  static bool deleteBookmark(const std::string& bookMd5, int index);

  // 计算书籍文件路径的 MD5 哈希
  static std::string calculateBookMd5(const std::string& filePath);

  // 加载所有书籍的书签
  static std::vector<Bookmark> loadAllBookmarks();

  // 检查是否存在任何书签
  static bool hasAnyBookmarks();

 private:
  static constexpr uint8_t FILE_VERSION = 1;
  static constexpr uint8_t MAX_BOOKMARKS = 50;
  static std::string getBookmarkFilePath(const std::string& bookMd5);
  static std::string getBookmarkDir();
};
