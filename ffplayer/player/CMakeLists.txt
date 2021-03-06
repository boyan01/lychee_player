cmake_minimum_required(VERSION 3.10)

project("media_player")

aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR} BASE_SOURCES)

add_library(media_player ${BASE_SOURCES})

target_include_directories(media_player PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(media_player PUBLIC media_base media_audio media_video)

if (MEDIA_USE_SDL)
    target_link_libraries(media_player PUBLIC media_sdl)
endif ()

