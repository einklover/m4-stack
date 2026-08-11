#pragma once
#include <memory>

#include "../ActivityWithSubactivity.h"
#include "TxtToEpubConverter.h"
#include "activities/home/MyLibraryActivity.h"

class Epub;
class Xtc;
class Txt;

class ReaderActivity final : public ActivityWithSubactivity {
  std::string initialBookPath;
  std::string currentBookPath;  // Track current book path for navigation
  const std::function<void()> onGoBack;
  const std::function<void(const std::string&)> onGoToLibrary;
  const std::string originalSourcePath;  // 原始源文件路径（如 TXT 文件），为空表示直接打开的就是 initialBookPath
  static std::unique_ptr<Epub> loadEpub(const std::string& path);
  static std::unique_ptr<Xtc> loadXtc(const std::string& path);
  static std::unique_ptr<Txt> loadTxt(const std::string& path);
  std::unique_ptr<Txt> loadTxtWithConversion(const std::string& path);
  static bool isXtcFile(const std::string& path);
  static bool isTxtFile(const std::string& path);
  static bool needsGbkToUtf8Conversion(const std::string& path);

  static std::string extractFolderPath(const std::string& filePath);
  void goToLibrary(const std::string& fromBookPath = "");
  void onGoToEpubReader(std::unique_ptr<Epub> epub);
  void onGoToXtcReader(std::unique_ptr<Xtc> xtc);
  void onGoToTxtReader(std::unique_ptr<Txt> txt);

 public:
  explicit ReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialBookPath,
                          const std::function<void()>& onGoBack,
                          const std::function<void(const std::string&)>& onGoToLibrary,
                          const std::string& originalSourcePath = "")
      : ActivityWithSubactivity("Reader", renderer, mappedInput),
        initialBookPath(std::move(initialBookPath)),
        onGoBack(onGoBack),
        onGoToLibrary(onGoToLibrary),
        originalSourcePath(originalSourcePath) {}
  void onEnter() override;
  bool isReaderActivity() const override { return true; }
  // This class is only the reader router/owner. The actual EPUB/TXT/XTC child
  // enables side-light gestures after it enters.
  bool isReaderBodyActivity() const override { return false; }
  // Delegate preventAutoSleep to the inner reader (EpubReaderActivity / XtcReaderActivity)
  bool preventAutoSleep() override { return subActivity && subActivity->preventAutoSleep(); }
};
