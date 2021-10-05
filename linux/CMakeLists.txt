cmake_minimum_required(VERSION 3.10)
set(PROJECT_NAME "lychee_player")
project(${PROJECT_NAME} LANGUAGES CXX)

set(CMAKE_CXX_FLAGS  "-fPIC ${CMAKE_CXX_FLAGS}")

# This value is used when generating builds using this plugin, so it must
# not be changed
set(PLUGIN_NAME "lychee_player_plugin")

add_library(${PLUGIN_NAME} SHARED
        "lychee_player_plugin.cc"
        )

# media_framework
set(MEDIA_BUILD_EXAMPLE OFF)
set(DISABLE_MEDIA_TEST ON)
set(MEDIA_USE_SDL "1")

add_subdirectory(../ffplayer media_framework)

apply_standard_settings(${PLUGIN_NAME})
set_target_properties(${PLUGIN_NAME} PROPERTIES
        CXX_VISIBILITY_PRESET hidden)
target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)
target_include_directories(${PLUGIN_NAME} INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(${PLUGIN_NAME} PRIVATE flutter media_flutter)
target_link_libraries(${PLUGIN_NAME} PRIVATE PkgConfig::GTK)

# List of absolute paths to libraries that should be bundled with the plugin
set(lychee_player_bundled_libraries
        ""
        "$<TARGET_FILE:media_flutter>"
        PARENT_SCOPE
        )
