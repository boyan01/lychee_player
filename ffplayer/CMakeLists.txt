cmake_minimum_required(VERSION 3.10)

if (WIN32)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
    set(BUILD_SHARED_LIBS TRUE)
    if (!CMAKE_TOOLCHAIN_FILE)
        set(CMAKE_TOOLCHAIN_FILE ${CMAKE_TOOLCHAIN_FILE} "$ENV{VCPKG_ROOT}//scripts//buildsystems//vcpkg.cmake")
    endif ()
    set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} "$ENV{VCPKG_ROOT}//installed//x64-windows")
    set(CMAKE_LIBRARY_PATH ${CMAKE_LIBRARY_PATH} "$ENV{VCPKG_ROOT}//installed//x64-windows//lib//manual-link")
    set(CMAKE_SYSTEM_VERSION "10")
    set(CMAKE_SYSTEM_NAME "WindowsStore")
endif ()

project("ffp" "CXX" "C")
set(CMAKE_CXX_STANDARD 14)

if (WIN32)
    set(VCPKG_HEADER_DIR "$ENV{VCPKG_ROOT}//installed//x64-windows//include")
    include_directories("${VCPKG_HEADER_DIR}")
endif (WIN32)

if (UNIX)
    # https://bugs.launchpad.net/ubuntu/+source/libsdl2-ttf/+bug/1872023
    include_directories("/usr/include/SDL2")
endif ()

if (APPLE)
    include_directories("/usr/local/include")
endif ()


add_library(ffplayer SHARED
        src/ffp_define.h
        src/ffp_msg_queue.h
        src/ffp_msg_queue.cc
        src/ffp_packet_queue.h
        src/ffp_packet_queue.cc
        src/ffp_decoder.h
        src/ffp_decoder.cc
        src/render_base.h
        src/ffp_frame_queue.h
        src/ffp_frame_queue.cc
        src/ffp_video_render.h
        src/ffp_video_render.cc
        src/ffp_audio_render.h
        src/ffp_audio_render.cc
        src/ffp_clock.h
        src/ffp_clock.cc
        src/ffp_data_source.h
        src/ffp_data_source.cc
        src/ffp_utils.h
        src/ffp_utils.cc
        src/ffp_player.h
        src/ffp_player.cc
        src/ffp_flutter.h
        src/ffp_flutter.cc
        src/ffp_flutter_windows.h
        src/ffp_flutter_windows.cc
        src/ffplayer.h
        src/decoder_base.h
        src/decoder_base.cc
        src/decoder_audio.h
        src/decoder_audio.cc
        src/decoder_video.h
        src/decoder_video.cc
        "third_party/dart/dart_api_dl.c"
        )

target_include_directories(ffplayer PRIVATE third_party)

find_library(LIBSDL2 SDL2 REQUIRED)

find_library(LIBAVUTIL avutil REQUIRED)
find_library(LIBAVCODEC avcodec REQUIRED)
find_library(LIBAVDEVICE avdevice REQUIRED)
find_library(LIBAVFORMAT avformat REQUIRED)
find_library(LIBAVFILTER avfilter REQUIRED)
find_library(LIBSWSCALE swscale REQUIRED)
find_library(LIBSWRESAMPLE swresample REQUIRED)

list(APPEND FFP_LIBS ${LIBAVUTIL}
        ${LIBAVCODEC}
        ${LIBAVDEVICE}
        ${LIBAVFORMAT}
        ${LIBAVFILTER}
        ${LIBSWSCALE}
        ${LIBSWRESAMPLE}
        ${LIBSDL2}
        )
if (FLUTTER)
    list(APPEND FFP_LIBS flutter)
endif ()

if (CMAKE_FLUTTER_WINDOWS)
    list(APPEND FFP_LIBS flutter_wrapper_plugin)
endif ()

if (UNIX)
    # linux do not have math library.
    list(APPEND FFP_LIBS m)
endif ()
target_link_libraries(ffplayer PUBLIC ${FFP_LIBS})

if (FLUTTER)
    # ignore example when if we build for flutter
else ()
    find_library(LIBSDL2MAIN SDL2main REQUIRED)
    add_executable(ffplay_example "src/ffplay.cc")
    add_dependencies(ffplay_example ffplayer)
    target_include_directories(ffplay_example PUBLIC include)
    target_link_libraries(ffplay_example PUBLIC ffplayer ${LIBSDL2MAIN})

    if (WIN32)
        if (CMAKE_FLUTTER_WINDOWS)
            set_property(TARGET ffplay_example APPEND PROPERTY LINK_FLAGS "/NODEFAULTLIB:MSVCRT")
        endif ()
    endif ()
endif ()
