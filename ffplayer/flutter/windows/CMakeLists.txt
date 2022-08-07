cmake_minimum_required(VERSION 3.10)

project("media_flutter_windows")

aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR} MEDIA_FLUTTER_WINDOWS_SOURCES)

add_library(media_flutter_windows ${MEDIA_FLUTTER_WINDOWS_SOURCES})

target_include_directories(media_flutter_windows PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(media_flutter_windows PUBLIC media_player media_flutter_base flutter flutter_wrapper_plugin)