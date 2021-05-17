cmake_minimum_required(VERSION 3.10)

project("media_flutter_base")

add_library(media_flutter_base
        flutter_video_renderer_sink.h
        flutter_video_renderer_sink.cpp
        null_video_renderer_sink.h
        null_video_renderer_sink.cc
        )

target_include_directories(media_flutter_base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(media_flutter_base PUBLIC media_player)