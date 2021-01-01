
#include <iostream>

extern "C" {
#include "libavutil/time.h"
}

using namespace std;

int main() {
  cout << "Hello CMake 1." << av_gettime_relative() << endl;
  return 0;
}