cmake_minimum_required (VERSION 3.10)

project ("ffplayer")

set(FFMPEG_INCLUDE_DIR "C:/Users/boyan/dev/vcpkg/installed/x64-windows/include")
set(FFMPEG_LIBRARY "C:/Users/boyan/dev/vcpkg/installed/x64-windows/include")

link_directories(${FFMPEG_LIBRARY})

include_directories("include" ${FFMPEG_INCLUDE_DIR})

add_executable (ffplayer "ffplay.cpp" "ffplayer.cpp")

target_link_libraries(ffplayer avutil avcodec avdevice avformat avfilter swscale SDL2)

# TODO: 如有需要，请添加测试并安装目标。
