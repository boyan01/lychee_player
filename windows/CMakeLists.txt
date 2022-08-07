cmake_minimum_required(VERSION 3.15)
set(PROJECT_NAME "lychee_player")
project(${PROJECT_NAME} LANGUAGES CXX)

# This value is used when generating builds using this plugin, so it must
# not be changed
set(PLUGIN_NAME "lychee_player_plugin")

add_library(${PLUGIN_NAME} SHARED
        "media_player_plugin.cpp"
        )
apply_standard_settings(${PLUGIN_NAME})

set(LYCHEE_PLAYER_BUILD_SHARED_FLUTTER_PLUGIN ON)

add_subdirectory(../ffplayer ffplayer.out)

set_target_properties(${PLUGIN_NAME} PROPERTIES
        CXX_VISIBILITY_PRESET hidden)
target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)
target_include_directories(${PLUGIN_NAME} INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(${PLUGIN_NAME} PRIVATE flutter flutter_wrapper_plugin)

# List of absolute paths to libraries that should be bundled with the plugin
set(${PROJECT_NAME}_bundled_libraries
        "$<TARGET_FILE:lychee_player_flutter>"
        "$<TARGET_FILE:SDL2>"
        "$<TARGET_FILE:avutil>"
        "$<TARGET_FILE:avcodec>"
        "$<TARGET_FILE:avformat>"
        "$<TARGET_FILE:swresample>"
        "$<TARGET_FILE:swscale>"
        PARENT_SCOPE
        )
