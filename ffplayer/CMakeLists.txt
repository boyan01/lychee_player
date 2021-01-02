cmake_minimum_required (VERSION 3.10)

project ("ffplayer" C)

set(FFMPEG_INCLUDE_DIR "C:/Users/boyan/dev/vcpkg/installed/x64-windows/include")
set(FFMPEG_LIBRARY "C:/Users/boyan/dev/vcpkg/installed/x64-windows/lib")

link_directories(${FFMPEG_LIBRARY})
include_directories("include" ${FFMPEG_INCLUDE_DIR})
link_directories("C:/Users/boyan/dev/vcpkg/installed/x64-windows/lib/manual-link")

add_executable(ffplayer "ffplayer.c" "ffplay.cpp")


target_link_libraries(ffplayer avutil avcodec avdevice avformat avfilter swscale SDL2 SDL2main)
    
# target_link_libraries(main PRIVATE SDL2::SDL2 SDL2::SDL2main)

# TODO: 如有需要，请添加测试并安装目标。
