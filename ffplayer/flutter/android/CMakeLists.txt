cmake_minimum_required(VERSION 3.10)

project("media_flutter_android")

aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR} MEDIA_FLUTTER_ANDROID_SOURCES)

add_library(media_flutter_android ${MEDIA_FLUTTER_ANDROID_SOURCES})

target_include_directories(media_flutter_android PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(media_flutter_android PUBLIC media_player media_flutter_base)