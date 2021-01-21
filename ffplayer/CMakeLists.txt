cmake_minimum_required(VERSION 3.10)

project("ffplayer" C)

if (WIN32)
    set(FFMPEG_INCLUDE_DIR "C:/Users/boyan/dev/vcpkg/installed/x64-windows/include")
    set(FFMPEG_LIBRARY "C:/Users/boyan/dev/vcpkg/installed/x64-windows/lib")

    link_directories(${FFMPEG_LIBRARY})
    include_directories("include" ${FFMPEG_INCLUDE_DIR})
    link_directories("C:/Users/boyan/dev/vcpkg/installed/x64-windows/lib/manual-link")
endif (WIN32)

if (UNIX)
    # https://bugs.launchpad.net/ubuntu/+source/libsdl2-ttf/+bug/1872023
    include_directories("/usr/include/SDL2")
endif ()

add_library(ffplayer SHARED "ffplayer.c" "utils.c")

add_library(ffplayer_static "ffplayer.c" "utils.c")
add_executable(ffplay_example "ffplay.c")


target_include_directories(ffplayer PUBLIC include)

target_link_libraries(ffplayer PUBLIC avutil avcodec avdevice avformat avfilter swscale swresample SDL2)

# linux do not have math library. 
if (UNIX)
target_link_libraries(ffplayer m)
endif(UNIX)

target_link_libraries(ffplay_example PRIVATE ffplayer_static SDL2main)

# TODO: 如有需要，请添加测试并安装目标。
