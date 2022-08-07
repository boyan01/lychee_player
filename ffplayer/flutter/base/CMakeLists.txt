cmake_minimum_required(VERSION 3.10)

project("media_flutter_base")

aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR} MEDIA_FLUTTER_BASE_SOURCES)

add_library(media_flutter_base ${MEDIA_FLUTTER_BASE_SOURCES})

target_include_directories(media_flutter_base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(media_flutter_base PUBLIC media_player)