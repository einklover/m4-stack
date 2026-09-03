#pragma once

#include "M4RuntimeMemorySnapshot.h"

M4RuntimeMemorySnapshot m4CaptureRuntimeMemory();
void m4LogRuntimeMemory(const char* stage);
