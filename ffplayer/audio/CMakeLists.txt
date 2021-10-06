cmake_minimum_required(VERSION 3.10)

project("media_audio")

list(APPEND audio_libraries media_player)

if (WIN32)
    list(APPEND audio_libraries avrt)
    list(APPEND audio_src_files wasapi_audio_render_sink.cc wasapi_audio_render_sink.h)
endif ()

add_library(media_audio ${audio_src_files})
target_include_directories(media_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(media_audio PUBLIC ${audio_libraries})