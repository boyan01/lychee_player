cmake_minimum_required(VERSION 3.15)
set(PROJECT_NAME "audio_player")
project(${PROJECT_NAME} LANGUAGES CXX)

# This value is used when generating builds using this plugin, so it must
# not be changed
set(PLUGIN_NAME "audio_player_plugin")

link_directories("C:/Users/boyan/dev/vcpkg/installed/x64-windows/lib")
link_directories("C:/Users/boyan/dev/vcpkg/installed/x64-windows/lib/manual-link")

add_library(${PLUGIN_NAME} SHARED
  "audio_player_plugin.cpp" "client_audio_player.cpp"
)
apply_standard_settings(${PLUGIN_NAME})

add_subdirectory(../ffplayer ffplayer.out)

set_target_properties(${PLUGIN_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden)
target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)
target_include_directories(${PLUGIN_NAME} INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(${PLUGIN_NAME} PRIVATE flutter flutter_wrapper_plugin
  # MfPlayer
  mfplay.lib shlwapi.lib
)

# List of absolute paths to libraries that should be bundled with the plugin
set(audio_player_bundled_libraries
  "$<TARGET_FILE:ffplayer>"
  PARENT_SCOPE
)
