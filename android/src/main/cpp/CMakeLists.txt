cmake_minimum_required(VERSION 3.12)

project("media_player" "CXX" "C")

message("ANDROID_STL = ${ANDROID_STL}")

add_library(media_player_android SHARED
        media_player_plugin.h
        media_player_plugin.cpp)

find_library(LOG_LIB log REQUIRED)

set(CMAKE_FLUTTER_MEDIA "1")
set(CMAKE_FLUTTER_MEDIA_ANDROID "1")

add_compile_definitions(_FLUTTER_MEDIA_)
add_compile_definitions(_FLUTTER_MEDIA_ANDROID)

set(CMAKE_LIBRARY_PATH ${CMAKE_LIBRARY_PATH} "${CMAKE_SOURCE_DIR}/lib/${CMAKE_ANDROID_ARCH}")

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

find_package(oboe REQUIRED CONFIG)

list(APPEND FFP_LIBS log cpufeatures_shared android oboe::oboe)

add_subdirectory(../../../../ffplayer ffplayer.out)

target_link_libraries(
        media_player_android
        ${log-lib}
        media_flutter
)