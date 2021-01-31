cmake_minimum_required(VERSION 3.10)

if (WIN32)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
    set(BUILD_SHARED_LIBS TRUE)
    if (!CMAKE_TOOLCHAIN_FILE)
        set(CMAKE_TOOLCHAIN_FILE ${CMAKE_TOOLCHAIN_FILE} "$ENV{VCPKG_ROOT}//scripts//buildsystems//vcpkg.cmake")
    endif()
    set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} "$ENV{VCPKG_ROOT}//installed//x64-windows")
    set(CMAKE_LIBRARY_PATH ${CMAKE_LIBRARY_PATH} "$ENV{VCPKG_ROOT}//installed//x64-windows//lib//manual-link")
    set(CMAKE_SYSTEM_VERSION "10")
    set(CMAKE_SYSTEM_NAME "WindowsStore")
endif ()

project("ffplayer" "CXX" "C")
set(CMAKE_CXX_STANDARD 17)

if (WIN32)
    set(VCPKG_HEADER_DIR "$ENV{VCPKG_ROOT}//installed//x64-windows//include")
    include_directories("${VCPKG_HEADER_DIR}")
endif (WIN32)

if (UNIX)
    # https://bugs.launchpad.net/ubuntu/+source/libsdl2-ttf/+bug/1872023
    include_directories("/usr/include/SDL2")
endif ()

add_library(ffplayer SHARED "ffplayer.c"
        "include/third_party/dart/dart_api_dl.c"
        "flutter.cpp"
        )

find_library(LIBSDL2 SDL2 REQUIRED)
find_library(LIBSDL2MAIN SDL2main REQUIRED)

target_include_directories(ffplayer PUBLIC include)

if (WIN32)
    find_library(LIBAVUTIL avutil REQUIRED)
    find_library(LIBAVCODEC avcodec REQUIRED)
    find_library(LIBAVDEVICE avdevice REQUIRED)
    find_library(LIBAVFORMAT avformat REQUIRED)
    find_library(LIBAVFILTER avfilter REQUIRED)
    find_library(LIBSWSCALE swscale REQUIRED)
    find_library(LIBSWRESAMPLE swresample REQUIRED)

    target_link_libraries(ffplayer PUBLIC
            ${LIBAVUTIL}
            ${LIBAVCODEC}
            ${LIBAVDEVICE}
            ${LIBAVFORMAT}
            ${LIBAVFILTER}
            ${LIBSWSCALE}
            ${LIBSWRESAMPLE}
            ${LIBSDL2})
else ()
    target_link_libraries(ffplayer PUBLIC avutil avcodec avdevice avformat avfilter swscale swresample ${LIBSDL2}
            # linux do not have math library.
            m)
endif ()

add_executable(ffplay_example "ffplay.cc")
add_dependencies(ffplay_example ffplayer)
target_include_directories(ffplay_example PUBLIC include)
target_link_libraries(ffplay_example PUBLIC ffplayer ${LIBSDL2MAIN})
if(MSVC)
	set_property(TARGET ffplay_example APPEND PROPERTY LINK_FLAGS "/NODEFAULTLIB:MSVCRT")
endif()
