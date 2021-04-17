cmake_minimum_required(VERSION 3.10)

project("media_sdl")

aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR} SDL_SOURCES)

add_library(media_sdl ${SDL_SOURCES}
        sdl_audio_renderer.cc
        )

target_include_directories(media_sdl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} include)

target_link_libraries(media_sdl PUBLIC media_player)