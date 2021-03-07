cmake_minimum_required(VERSION 3.10)
set(PROJECT_NAME "av_player")

set(CMAKE_CXX_FLAGS  "-fPIC ${CMAKE_CXX_FLAGS}")

message("CMAKE_CXX_FLAGS = ${CMAKE_CXX_FLAGS}")

project(${PROJECT_NAME} LANGUAGES CXX)

# This value is used when generating builds using this plugin, so it must
# not be changed
set(PLUGIN_NAME "${PROJECT_NAME}_plugin")

add_library(${PLUGIN_NAME} SHARED
        "av_player_plugin.cc"
        )

set(CMAKE_FLUTTER_MEDIA "1")
set(CMAKE_FLUTTER_MEDIA_LINUX "1")
set(MEDIA_USE_SDL "1")

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
target_link_libraries(${PLUGIN_NAME} PRIVATE media_flutter)

# List of absolute paths to libraries that should be bundled with the plugin
set(${PROJECT_NAME}_bundled_libraries
        "$<TARGET_FILE:media_flutter>"
        PARENT_SCOPE
        )
