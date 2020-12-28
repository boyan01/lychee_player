cmake_minimum_required (VERSION 3.8)

project ("ffplayer")

set(FFMPEG_INCLUDE_DIR "C:/Users/boyan/dev/vcpkg/packages/ffmpeg_x64-windows/include")
set(FFMPEG_LIBRARY "C:/Users/boyan/dev/vcpkg/packages/ffmpeg_x64-windows/debug/lib")
include_directories(${FFMPEG_INCLUDE_DIR})
link_directories(${FFMPEG_LIBRARY})


add_executable (ffplayer "ffplayer.cpp" "ffplayer.h")

target_link_libraries(ffplayer avutil avcodec avdevice avformat avfilter swscale)

# TODO: 如有需要，请添加测试并安装目标。
