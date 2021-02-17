cmake_minimum_required(VERSION 3.10)

project("av_player" "CXX" "C")

add_subdirectory(SDL)

add_library(av_player SHARED
        av_player_plugin.h
        av_player_plugin.cpp)

message("${PROJECT_NAME} ANDROID_UNIFIED_HEADERS = ${ANDROID_UNIFIED_HEADERS}")
message(" CMAKE_FIND_ROOT_PATH  = ${CMAKE_FIND_ROOT_PATH}")

find_library(LOG_LIB log REQUIRED)

set(FLUTTER "1")
add_compile_definitions(_FLUTTER)
add_compile_definitions(_FLUTTER_ANDROID)

message("ANDROID_ABI = ${ANDROID_ABI}, CMAKE_SOURCE_DIR = ")

set(CMAKE_LIBRARY_PATH ${CMAKE_LIBRARY_PATH} "${CMAKE_SOURCE_DIR}/lib/${CMAKE_ANDROID_ARCH}")
set(CMAKE_FLUTTER_ANDROID_LINK_PATH "${CMAKE_SOURCE_DIR}/lib/${CMAKE_ANDROID_ARCH}")

list(APPEND FFP_LIBS
        avutil
        avcodec
        avdevice
        avformat
        avfilter
        swscale
        swresample
        c++_shared
        )

set(FFP_ANDROID_LIBS "${CMAKE_SOURCE_DIR}/lib/${CMAKE_ANDROID_ARCH_ABI}")
foreach (LIB IN LISTS FFP_LIBS)
    add_library(${LIB} SHARED IMPORTED)
    set_target_properties(${LIB} PROPERTIES IMPORTED_LOCATION "${FFP_ANDROID_LIBS}/lib${LIB}.so")
endforeach ()

add_library(cpufeatures_shared SHARED IMPORTED)
set_target_properties(cpufeatures_shared PROPERTIES IMPORTED_LOCATION "${FFP_ANDROID_LIBS}/libcpufeatures.so")

list(APPEND FFP_LIBS log SDL2 cpufeatures_shared)

add_subdirectory(ffplayer ffplayer.out)

target_link_libraries(
        av_player
        ${log-lib}
        ffplayer
)