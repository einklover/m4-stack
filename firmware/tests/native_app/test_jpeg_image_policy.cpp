#include "converters/JpegImagePolicy.h"

#include <cassert>
#include <iostream>

int main() {
  using namespace JpegImagePolicy;
  assert(validSourceDimensions(800, 1280));
  assert(validSourceDimensions(4000, 2000));
  assert(!validSourceDimensions(0, 10));
  assert(!validSourceDimensions(10, 0));
  assert(!validSourceDimensions(4097, 1));
  assert(!validSourceDimensions(3000, 3000));
  assert(validDestinationDimensions(480, 800));
  assert(!validDestinationDimensions(0, 800));
  assert(!validDestinationDimensions(480, -1));
  size_t bytes = 0;
  assert(mcuRowBufferBytes(4096, 16, bytes) && bytes == 65536);
  assert(!mcuRowBufferBytes(4097, 16, bytes));
  assert(rgbaBufferBytes(800, 1280, bytes) && bytes == 4096000);
  assert(!rgbaBufferBytes(0, 1280, bytes));
  std::cout << "JPEG image policy PASS\n";
}
