# The Flutter tooling requires that developers have a version of Visual Studio
# installed that includes CMake 3.14 or later. The core needs 3.20, which
# Visual Studio 2022 ships.
cmake_minimum_required(VERSION 3.14)

set(PROJECT_NAME "lychee_player")
project(${PROJECT_NAME} LANGUAGES C CXX)
cmake_policy(VERSION 3.14...3.25)

# This value is used when generating builds using this plugin, so it must
# not be changed.
set(PLUGIN_NAME "lychee_player_plugin")

# LYCHEE_WITH_FFMPEG=OFF (cache or environment variable) builds the core
# without FFmpeg: Media Foundation and the core's own demuxers, no FFmpeg
# DLLs in the app (README, "Building the core").
# The environment wins over the cache, so changing it needs no clean build.
if(DEFINED ENV{LYCHEE_WITH_FFMPEG})
  set(LYCHEE_WITH_FFMPEG "$ENV{LYCHEE_WITH_FFMPEG}")
elseif(NOT DEFINED LYCHEE_WITH_FFMPEG)
  set(LYCHEE_WITH_FFMPEG ON)
endif()

# The pinned LGPL FFmpeg, unless one is given: the LYCHEE_FFMPEG_ROOT cache
# or environment variable, e.g. tool/build_ffmpeg.sh windows-x86_64's
# output.
include("${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg.cmake")
set(LYCHEE_FFMPEG_ROOT "" CACHE PATH
  "Prebuilt LGPL-only FFmpeg (include/, lib/, bin/); downloaded when empty")
if(NOT LYCHEE_FFMPEG_ROOT AND DEFINED ENV{LYCHEE_FFMPEG_ROOT})
  file(TO_CMAKE_PATH "$ENV{LYCHEE_FFMPEG_ROOT}" LYCHEE_FFMPEG_ROOT)
endif()
if(LYCHEE_WITH_FFMPEG AND NOT LYCHEE_FFMPEG_ROOT)
  lychee_fetch_ffmpeg("${CMAKE_BINARY_DIR}/ffmpeg" LYCHEE_FFMPEG_ROOT)
endif()

# The core as lychee_player.dll: Dart binds its C ABI through FFI, and the
# texture bridge below links it. Its source is the repository's core/ in a
# checkout, else the package's core/, the copy that tool/release.sh puts
# there for pub to publish. Apps reach the package through a symlink
# (.plugin_symlinks/lychee_player); Windows collapses ../ lexically and
# CMake's REALPATH does not follow it there, so read the link.
get_filename_component(package_dir "${CMAKE_CURRENT_SOURCE_DIR}" DIRECTORY)
if(IS_SYMLINK "${package_dir}")
  get_filename_component(link_parent "${package_dir}" DIRECTORY)
  file(READ_SYMLINK "${package_dir}" package_dir)
  # Windows reports the target as an NT path: \??\C:\...
  string(REGEX REPLACE "^[\\/][?][?][\\/]" "" package_dir "${package_dir}")
  file(TO_CMAKE_PATH "${package_dir}" package_dir)
  if(NOT IS_ABSOLUTE "${package_dir}")
    set(package_dir "${link_parent}/${package_dir}")
  endif()
endif()
set(BUILD_SHARED_LIBS ON)
set(LYCHEE_WARNINGS_AS_ERRORS ON)
set(core_dir "${package_dir}/../core")
if(NOT IS_DIRECTORY "${core_dir}")
  set(core_dir "${package_dir}/core")
endif()
add_subdirectory("${core_dir}" "${CMAKE_CURRENT_BINARY_DIR}/core")
set_target_properties(lychee PROPERTIES OUTPUT_NAME lychee_player)

add_library(${PLUGIN_NAME} SHARED
  "include/lychee_player/lychee_player_plugin_c_api.h"
  "lychee_player_plugin_c_api.cpp"
  "lychee_player_plugin.cpp"
  "lychee_player_plugin.h"
  "lychee_texture.cpp"
  "lychee_texture.h"
)
apply_standard_settings(${PLUGIN_NAME})
set_target_properties(${PLUGIN_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden)
target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)
target_include_directories(${PLUGIN_NAME} INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(${PLUGIN_NAME} PRIVATE
  flutter flutter_wrapper_plugin lychee d3d11 dxgi)

# Shipped next to the app: the core and the FFmpeg libraries it links.
set(ffmpeg_dlls "")
if(LYCHEE_WITH_FFMPEG)
  foreach(lib ${LYCHEE_FFMPEG_LIBRARIES})
    file(GLOB dll "${LYCHEE_FFMPEG_ROOT}/bin/${lib}-*.dll")
    list(APPEND ffmpeg_dlls ${dll})
  endforeach()
endif()
set(lychee_player_bundled_libraries
  $<TARGET_FILE:lychee>
  ${ffmpeg_dlls}
  PARENT_SCOPE
)
