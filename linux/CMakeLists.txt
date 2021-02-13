cmake_minimum_required(VERSION 3.10)
set(PROJECT_NAME "av_player")
project(${PROJECT_NAME} LANGUAGES CXX)

# This value is used when generating builds using this plugin, so it must
# not be changed
set(PLUGIN_NAME "${PROJECT_NAME}_plugin")

add_library(${PLUGIN_NAME} SHARED
        "av_player_plugin.cc"
        )

set(FLUTTER "1")
add_compile_definitions(_FLUTTER)
add_compile_definitions(_FLUTTER_LINUX)
add_subdirectory(../ffplayer ffplayer.out)

set_target_properties(ffplayer
        PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        )

apply_standard_settings(${PLUGIN_NAME})
set_target_properties(${PLUGIN_NAME} PROPERTIES
        CXX_VISIBILITY_PRESET hidden)
target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)
target_include_directories(${PLUGIN_NAME} INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(${PLUGIN_NAME} PRIVATE flutter)
target_link_libraries(${PLUGIN_NAME} PRIVATE PkgConfig::GTK)
target_link_libraries(${PLUGIN_NAME} PRIVATE ffplayer)

add_dependencies(${PLUGIN_NAME} ffplayer)

# List of absolute paths to libraries that should be bundled with the plugin
set(${PROJECT_NAME}_bundled_libraries
        "$<TARGET_FILE:ffplayer>"
        PARENT_SCOPE
        )
