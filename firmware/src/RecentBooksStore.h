#pragma once
#include <string>
#include <vector>

struct RecentBook {
  std::string path;              // 实际打开的文件路径（可能是缓存的 EPUB）
  std::string title;
  std::string author;
  std::string coverBmpPath;
  std::string originalSourcePath;  // 原始源文件路径（如 TXT 文件），为空表示直接打开的就是 path
  int progress = 0;               // 阅读进度百分比 (0-100)
  int totalReadingTime = 0;       // 总阅读时长（秒）

  bool operator==(const RecentBook& other) const { return path == other.path; }
};

class RecentBooksStore {
  // Static instance
  static RecentBooksStore instance;

  std::vector<RecentBook> recentBooks;

 public:
  ~RecentBooksStore() = default;

  // Get singleton instance
  static RecentBooksStore& getInstance() { return instance; }

  // Add a book to the recent list (moves to front if already exists)
  // originalSourcePath: 原始源文件路径（如 TXT 文件），为空表示直接打开的就是 path
  void addBook(const std::string& path, const std::string& title, const std::string& author,
               const std::string& coverBmpPath, const std::string& originalSourcePath = "");

  void updateBook(const std::string& path, const std::string& title, const std::string& author,
                  const std::string& coverBmpPath);

  // Provider metadata is best-effort and arrives after the history row may
  // already exist. Empty values preserve the current row, so failed cover
  // acquisition is non-fatal and cannot erase good metadata.
  void updateProviderBook(const std::string& path, const std::string& title, const std::string& author,
                          const std::string& coverBmpPath);

  // Get the list of recent books (most recent first)
  const std::vector<RecentBook>& getBooks() const { return recentBooks; }

  // Get the count of recent books
  int getCount() const { return static_cast<int>(recentBooks.size()); }

  bool saveToFile() const;

  bool loadFromFile();
  RecentBook getDataFromBook(std::string path) const;
};

// Helper macro to access recent books store
#define RECENT_BOOKS RecentBooksStore::getInstance()
