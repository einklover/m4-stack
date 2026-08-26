#pragma once

class FsFile {
 public:
  void close() {}
};

class SDCardManager {
 public:
  bool mkdir(const char*) { return true; }
  bool openFileForWrite(const char*, const char*, FsFile&) { return false; }
  bool openFileForRead(const char*, const char*, FsFile&) { return false; }
};

inline SDCardManager SdMan;
