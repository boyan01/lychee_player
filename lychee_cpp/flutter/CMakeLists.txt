cmake_minimum_required(VERSION 3.15)

project("lychee_player_plugin" "CXX" "C")

add_library("lychee_player_plugin"
        lychee_player_plugin.h
        lychee_player_plugin.cc
        third_party/dart/dart_api_dl.c
        )

target_link_libraries(lychee_player_plugin PUBLIC "lychee_player")
target_include_directories(lychee_player_plugin PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} third_party)

