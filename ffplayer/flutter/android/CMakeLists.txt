cmake_minimum_required(VERSION 3.10)

project("media_flutter_android")

add_library(media_flutter_android
        oboe_audio_renderer_sink.h
        oboe_audio_renderer_sink.cc
        )

target_include_directories(media_flutter_android PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(media_flutter_android PUBLIC media_player media_flutter_base)