cmake_minimum_required(VERSION 3.10)

if (WIN32)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
    #    set(BUILD_SHARED_LIBS TRUE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} "$ENV{VCPKG_ROOT}//installed//x64-windows")
    set(CMAKE_LIBRARY_PATH ${CMAKE_LIBRARY_PATH} "$ENV{VCPKG_ROOT}//installed//x64-windows//lib//manual-link")
    set(CMAKE_SYSTEM_VERSION "10")
    set(CMAKE_SYSTEM_NAME "WindowsStore")
endif ()

project("media" "CXX" "C")
set(CMAKE_CXX_STANDARD 14)

if (NOT DEFINED FFP_LIBS)
    find_library(LIBAVUTIL avutil REQUIRED)
    find_library(LIBAVCODEC avcodec REQUIRED)
    find_library(LIBAVDEVICE avdevice REQUIRED)
    find_library(LIBAVFORMAT avformat REQUIRED)
    find_library(LIBAVFILTER avfilter REQUIRED)
    find_library(LIBSWSCALE swscale REQUIRED)
    find_library(LIBSWRESAMPLE swresample REQUIRED)

    find_library(LIBSDL2 SDL2 REQUIRED)

    list(APPEND FFP_LIBS ${LIBAVUTIL}
            ${LIBAVCODEC}
            ${LIBAVDEVICE}
            ${LIBAVFORMAT}
            ${LIBAVFILTER}
            ${LIBSWSCALE}
            ${LIBSWRESAMPLE}
            ${LIBSDL2}
            )
    if (UNIX)
        # linux do not have math library.
        list(APPEND FFP_LIBS m pthread)
    endif ()
endif ()

add_subdirectory(base)
add_subdirectory(audio)
add_subdirectory(video)

if (MEDIA_USE_SDL OR NOT CMAKE_FLUTTER_MEDIA)
    add_subdirectory(sdl)
endif ()

add_subdirectory(player)

if (CMAKE_FLUTTER_MEDIA)
    add_subdirectory(flutter)
else ()
    add_subdirectory(example)
endif ()

message("VCPKG_APPLOCAL_DEPS = ${VCPKG_APPLOCAL_DEPS}")
message("VCPKG_APPINSTALL_DEPS  = ${VCPKG_APPINSTALL_DEPS}")