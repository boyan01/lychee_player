cmake_minimum_required(VERSION 3.14)

project("lychee_player_plugin" "CXX" "C")

if (LYCHEE_PLAYER_BUILD_SHARED_FLUTTER_PLUGIN)
  message("Building shared flutter plugin")
  add_library("lychee_player_plugin" SHARED
    lychee_player_plugin.h
    lychee_player_plugin.cc
    third_party/dart/dart_api_dl.c
    )
else ()
  message("Building static flutter plugin")
  add_library("lychee_player_plugin" STATIC
    lychee_player_plugin.h
    lychee_player_plugin.cc
    third_party/dart/dart_api_dl.c
    )
endif ()


target_link_libraries(lychee_player_plugin PUBLIC "lychee_player")
target_include_directories(lychee_player_plugin PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} third_party)

