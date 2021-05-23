cmake_minimum_required(VERSION 3.10)

project("media_macos")

add_library(media_macos
        macos_audio_renderer_sink.h
        macos_audio_renderer_sink.cc
        )

target_include_directories(media_macos PUBLIC .)

target_link_libraries(media_macos PUBLIC media_player "-framework AudioToolbox")