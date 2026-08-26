#pragma once

#include <string>

#include "SDCardManager.h"

namespace serialization {
template <typename T>
void writePod(FsFile&, const T&) {}

template <typename T>
void readPod(FsFile&, T&) {}

inline void writeString(FsFile&, const std::string&) {}
inline void readString(FsFile&, std::string&) {}
}  // namespace serialization
