# The Flutter tooling requires that developers have a version of Visual Studio
# installed that includes CMake 3.14 or later. You should not increase this
# version, as doing so will cause the plugin to fail to compile for some
# customers of the plugin.
cmake_minimum_required(VERSION 3.14)

set(PROJECT_NAME "lychee_player_flutter")
project(${PROJECT_NAME} LANGUAGES CXX)

set(LYCHEE_PLAYER_BUILD_SHARED_FLUTTER_PLUGIN ON)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../lychee_cpp" "lychee_cpp_shared")

set(lychee_player_bundled_libraries
  $<TARGET_FILE:lychee_player_plugin>
  "$<TARGET_FILE:SDL2>"
  "$<TARGET_FILE:avutil>"
  "$<TARGET_FILE:avcodec>"
  "$<TARGET_FILE:avformat>"
  "$<TARGET_FILE:swresample>"
  "$<TARGET_FILE:swscale>"
  PARENT_SCOPE
  )
