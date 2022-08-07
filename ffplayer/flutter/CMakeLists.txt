cmake_minimum_required(VERSION 3.14)

project("lychee_player_flutter" "CXX" "C")

if (LYCHEE_PLAYER_BUILD_SHARED_FLUTTER_PLUGIN)
    message("Building shared flutter plugin")
    add_library("lychee_player_flutter" SHARED
            lychee_player_plugin.h
            lychee_player_plugin.cc
            third_party/dart/dart_api_dl.c
            )
else ()
    message("Building static flutter plugin")
    add_library("lychee_player_flutter" STATIC
            lychee_player_plugin.h
            lychee_player_plugin.cc
            third_party/dart/dart_api_dl.c
            )
endif ()


target_link_libraries(lychee_player_flutter PUBLIC "lychee_player")
target_include_directories(lychee_player_flutter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} third_party)

