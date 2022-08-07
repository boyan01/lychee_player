cmake_minimum_required(VERSION 3.10)
set(PROJECT_NAME "lychee_player")

set(CMAKE_CXX_FLAGS  "-fPIC ${CMAKE_CXX_FLAGS}")

message("CMAKE_CXX_FLAGS = ${CMAKE_CXX_FLAGS}")

project(${PROJECT_NAME} LANGUAGES CXX)

# This value is used when generating builds using this plugin, so it must
# not be changed
set(PLUGIN_NAME "${PROJECT_NAME}_plugin")

add_library(${PLUGIN_NAME} SHARED
        "media_player_plugin.cc"
        )

set(LYCHEE_PLAYER_BUILD_SHARED_FLUTTER_PLUGIN ON)

add_compile_definitions(_FLUTTER_MEDIA_)
add_compile_definitions(_FLUTTER_MEDIA_LINUX)

add_subdirectory(../ffplayer ffplayer.out)

apply_standard_settings(${PLUGIN_NAME})
set_target_properties(${PLUGIN_NAME} PROPERTIES
        CXX_VISIBILITY_PRESET hidden)
target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)
target_include_directories(${PLUGIN_NAME} INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(${PLUGIN_NAME} PRIVATE flutter)
target_link_libraries(${PLUGIN_NAME} PRIVATE PkgConfig::GTK)

# List of absolute paths to libraries that should be bundled with the plugin
set(${PROJECT_NAME}_bundled_libraries
        "$<TARGET_FILE:lychee_player_flutter>"
        PARENT_SCOPE
        )
