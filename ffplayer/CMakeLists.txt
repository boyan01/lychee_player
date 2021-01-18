cmake_minimum_required(VERSION 3.10)

project("ffplayer" C)

if (WIN32)
    set(FFMPEG_INCLUDE_DIR "C:/Users/boyan/dev/vcpkg/installed/x64-windows/include")
    set(FFMPEG_LIBRARY "C:/Users/boyan/dev/vcpkg/installed/x64-windows/lib")

    link_directories(${FFMPEG_LIBRARY})
    include_directories("include" ${FFMPEG_INCLUDE_DIR})
    link_directories("C:/Users/boyan/dev/vcpkg/installed/x64-windows/lib/manual-link")
endif (WIN32)

add_library(ffplayer "ffplayer.c" "utils.c")
add_executable(ffplay_example "ffplay.c")


target_include_directories(ffplayer PUBLIC include)

target_link_libraries(ffplayer avutil avcodec avdevice avformat avfilter swscale swresample SDL2 SDL2main m)
target_link_libraries(ffplay_example PRIVATE ffplayer)

# target_link_libraries(main PRIVATE SDL2::SDL2 SDL2::SDL2main)

# TODO: 如有需要，请添加测试并安装目标。
