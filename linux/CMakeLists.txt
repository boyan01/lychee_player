# The Flutter tooling requires that developers have CMake 3.10 or later
# installed. You should not increase this version, as doing so will cause
# the plugin to fail to compile for some customers of the plugin.
cmake_minimum_required(VERSION 3.10)

set(PROJECT_NAME "lychee_player_flutter")
project(${PROJECT_NAME} LANGUAGES CXX)

set(CMAKE_CXX_FLAGS  "-fPIC ${CMAKE_CXX_FLAGS}")

set(LYCHEE_PLAYER_BUILD_SHARED_FLUTTER_PLUGIN ON)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../lychee_cpp" "lychee_cpp_shared")

set(lychee_player_bundled_libraries
  $<TARGET_FILE:lychee_player_plugin>
  PARENT_SCOPE
  )
