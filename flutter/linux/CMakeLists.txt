# The Linux plugin: liblychee_player.so (the whole core, exporting the C ABI
# that Dart opens through FFI), the GTK plugin that bridges players to
# Flutter textures, and an LGPL FFmpeg bundled next to them.
cmake_minimum_required(VERSION 3.20)

set(PROJECT_NAME "lychee_player")
project(${PROJECT_NAME} LANGUAGES C CXX)

set(PLUGIN_NAME "lychee_player_plugin")

# The core's source: the repository's core/ in a checkout, else the
# package's core/, the copy that tool/release.sh puts there for pub to
# publish. Flutter reaches this directory through a symlink in the app's
# flutter/ephemeral/.plugin_symlinks; resolve it before going up.
get_filename_component(_lychee_plugin_dir "${CMAKE_CURRENT_SOURCE_DIR}" REALPATH)
get_filename_component(_lychee_core "${_lychee_plugin_dir}/../../core" ABSOLUTE)
if(NOT IS_DIRECTORY "${_lychee_core}")
  get_filename_component(_lychee_core "${_lychee_plugin_dir}/../core" ABSOLUTE)
endif()

# Everything the bundle loads sits in one directory with RUNPATH $ORIGIN, in
# the build tree as in the app's bundle/lib.
set(_lychee_out "${CMAKE_CURRENT_BINARY_DIR}/lib")
file(MAKE_DIRECTORY "${_lychee_out}")

# --- FFmpeg ---------------------------------------------------------------
# A prebuilt LGPL FFmpeg (include/ and lib/ with shared libraries): the
# LYCHEE_FFMPEG_ROOT cache variable or environment variable, e.g.
# tool/build_ffmpeg.sh linux-x86_64's, or else the one prebuilt.json pins
# (ffmpeg.cmake), downloaded once into ~/.cache/lychee_player.
include("${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg.cmake")
set(LYCHEE_FFMPEG_ROOT "" CACHE PATH
  "Prebuilt FFmpeg (include/, lib/); the pinned LGPL build is downloaded when empty")
if(NOT LYCHEE_FFMPEG_ROOT AND DEFINED ENV{LYCHEE_FFMPEG_ROOT})
  set(LYCHEE_FFMPEG_ROOT "$ENV{LYCHEE_FFMPEG_ROOT}")
endif()

if(NOT LYCHEE_FFMPEG_ROOT)
  if(DEFINED ENV{XDG_CACHE_HOME} AND NOT "$ENV{XDG_CACHE_HOME}" STREQUAL "")
    set(_ffmpeg_cache "$ENV{XDG_CACHE_HOME}/lychee_player")
  else()
    set(_ffmpeg_cache "$ENV{HOME}/.cache/lychee_player")
  endif()
  file(MAKE_DIRECTORY "${_ffmpeg_cache}")
  # A normal variable, not cached: a deleted cache directory is downloaded
  # again on the next configure.
  lychee_fetch_ffmpeg("${_ffmpeg_cache}" LYCHEE_FFMPEG_ROOT)
endif()
message(STATUS "lychee_player: FFmpeg from ${LYCHEE_FFMPEG_ROOT}")

# Copy each library under its SONAME (lib*.so.N, what the loader asks for).
# configure_file follows the symlinks.
set(_lychee_ffmpeg_bundle)
foreach(_lib ${LYCHEE_FFMPEG_LIBRARIES})
  file(GLOB _candidates "${LYCHEE_FFMPEG_ROOT}/lib/lib${_lib}.so.[0-9]*")
  set(_soname "")
  foreach(_candidate ${_candidates})
    get_filename_component(_name "${_candidate}" NAME)
    if(_name MATCHES "^lib${_lib}\\.so\\.[0-9]+$")
      set(_soname "${_candidate}")
    endif()
  endforeach()
  if(NOT _soname)
    message(FATAL_ERROR "No lib${_lib}.so.N in ${LYCHEE_FFMPEG_ROOT}/lib")
  endif()
  get_filename_component(_name "${_soname}" NAME)
  configure_file("${_soname}" "${_lychee_out}/${_name}" COPYONLY)
  list(APPEND _lychee_ffmpeg_bundle "${_lychee_out}/${_name}")
endforeach()

# --- Core -----------------------------------------------------------------
set(BUILD_SHARED_LIBS ON)
add_subdirectory("${_lychee_core}" "${CMAKE_CURRENT_BINARY_DIR}/core")
unset(BUILD_SHARED_LIBS)
# Export the C ABI only: libstdc++'s inline templates would otherwise leak
# out with default visibility.
set(_lychee_exports "${CMAKE_CURRENT_BINARY_DIR}/lychee_player.map")
file(WRITE "${_lychee_exports}" "{ global: lychee_*; local: *; };\n")
target_link_options(lychee PRIVATE "-Wl,--version-script=${_lychee_exports}")
set_target_properties(lychee PROPERTIES
  LINK_DEPENDS "${_lychee_exports}"
  OUTPUT_NAME lychee_player
  LIBRARY_OUTPUT_DIRECTORY "${_lychee_out}"
  BUILD_WITH_INSTALL_RPATH ON
  INSTALL_RPATH "$ORIGIN"
)

# --- Plugin ---------------------------------------------------------------
add_library(${PLUGIN_NAME} SHARED lychee_player_plugin.cc)
apply_standard_settings(${PLUGIN_NAME})
set_target_properties(${PLUGIN_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  LIBRARY_OUTPUT_DIRECTORY "${_lychee_out}"
  BUILD_WITH_INSTALL_RPATH ON
  INSTALL_RPATH "$ORIGIN"
)
target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)
target_include_directories(${PLUGIN_NAME} INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
# libepoxy for EGL and GL, as Flutter's Linux embedder uses it.
find_package(PkgConfig REQUIRED)
pkg_check_modules(EPOXY REQUIRED IMPORTED_TARGET epoxy)
target_link_libraries(${PLUGIN_NAME} PRIVATE flutter PkgConfig::GTK
  PkgConfig::EPOXY lychee)

# Installed into the app's bundle/lib next to the plugin.
set(lychee_player_bundled_libraries
  $<TARGET_FILE:lychee>
  ${_lychee_ffmpeg_bundle}
  PARENT_SCOPE
)
