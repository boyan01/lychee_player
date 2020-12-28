// ffplayer.cpp: 定义应用程序的入口点。
//

#include "ffplayer.h"

extern "C" {
#include "libavutil/time.h"
}

using namespace std;

int main() {
  cout << "Hello CMake." << av_gettime_relative() << endl;
  return 0;
}
