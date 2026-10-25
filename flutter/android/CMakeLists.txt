cmake_minimum_required(VERSION 3.22.1)
project(lychee_player LANGUAGES C CXX)

set(LYCHEE_FFMPEG_ROOT
  "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/ffmpeg/android-${ANDROID_ABI}"
  CACHE PATH "")
add_subdirectory(../../core core)
# Debug apps build native code at -O0, too slow for per-pixel frame work.
target_compile_options(lychee PRIVATE -O2)

# One library for Dart (the C ABI, through FFI) and Kotlin (the texture
# bridge, through JNI). Whole-archive keeps every C ABI function, not just
# the ones the bridge calls.
add_library(lychee_player SHARED src/main/cpp/texture_bridge.cc)
target_link_libraries(lychee_player PRIVATE
  -Wl,--whole-archive lychee -Wl,--no-whole-archive
  android log)
target_link_options(lychee_player PRIVATE -Wl,-z,max-page-size=16384)
