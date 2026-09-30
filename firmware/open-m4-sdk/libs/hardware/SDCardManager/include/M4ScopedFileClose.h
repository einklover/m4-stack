#pragma once

// SdFat's default destructor does not close files. Protect exception paths
// without changing its global destructor policy or copying file handles.
template <class File>
class M4ScopedFileClose {
 public:
  explicit M4ScopedFileClose(File& file) : file_(file) {}
  ~M4ScopedFileClose() { if (file_) file_.close(); }
  M4ScopedFileClose(const M4ScopedFileClose&) = delete;
  M4ScopedFileClose& operator=(const M4ScopedFileClose&) = delete;
 private:
  File& file_;
};
