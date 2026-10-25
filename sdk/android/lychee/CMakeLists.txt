cmake_minimum_required(VERSION 3.22.1)
project(lychee_android LANGUAGES C CXX)

# The core as liblychee.so: the C ABI, for the Kotlin classes, Dart FFI and
# native callers (the AAR's prefab module).
set(LYCHEE_FFMPEG_ROOT
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/ffmpeg/android-${ANDROID_ABI}"
  CACHE PATH "")
set(BUILD_SHARED_LIBS ON)
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-z,max-page-size=16384")
add_subdirectory(../../../core core)
# Debug apps build native code at -O0, too slow for per-pixel frame work.
target_compile_options(lychee PRIVATE -O2)

# JNI for the Kotlin classes; uses only lychee.h.
add_library(lychee_jni SHARED src/main/cpp/lychee_jni.cc)
target_link_libraries(lychee_jni PRIVATE lychee android nativewindow log)
