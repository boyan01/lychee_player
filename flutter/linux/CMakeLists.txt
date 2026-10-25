# The Linux plugin: liblychee_player.so (the whole core, exporting the C ABI
# that Dart opens through FFI), the GTK plugin that bridges players to
# Flutter textures, and an LGPL FFmpeg bundled next to them.
cmake_minimum_required(VERSION 3.20)

set(PROJECT_NAME "lychee_player")
project(${PROJECT_NAME} LANGUAGES C CXX)

set(PLUGIN_NAME "lychee_player_plugin")

# Flutter reaches this directory through a symlink in the app's
# flutter/ephemeral/.plugin_symlinks; resolve it before going up to core/.
get_filename_component(_lychee_plugin_dir "${CMAKE_CURRENT_SOURCE_DIR}" REALPATH)
get_filename_component(_lychee_repo "${_lychee_plugin_dir}/../.." ABSOLUTE)

# Everything the bundle loads sits in one directory with RUNPATH $ORIGIN, in
# the build tree as in the app's bundle/lib.
set(_lychee_out "${CMAKE_CURRENT_BINARY_DIR}/lib")
file(MAKE_DIRECTORY "${_lychee_out}")

# --- FFmpeg ---------------------------------------------------------------
# A prebuilt LGPL FFmpeg (include/ and lib/ with shared libraries): the
# LYCHEE_FFMPEG_ROOT cache variable or environment variable, or else the
# pinned BtbN build below, downloaded once into ~/.cache/lychee_player.
# BtbN's lgpl-shared builds are configured with --enable-version3 and
# without --enable-gpl/--enable-nonfree: LGPL v3.
set(LYCHEE_FFMPEG_ROOT "" CACHE PATH
  "Prebuilt FFmpeg (include/, lib/); the pinned LGPL build is downloaded when empty")
if(NOT LYCHEE_FFMPEG_ROOT AND DEFINED ENV{LYCHEE_FFMPEG_ROOT})
  set(LYCHEE_FFMPEG_ROOT "$ENV{LYCHEE_FFMPEG_ROOT}")
endif()

if(NOT LYCHEE_FFMPEG_ROOT)
  set(_ffmpeg_release
    "https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-09-30-13-08")
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(_ffmpeg_name "ffmpeg-n9.0.2-17-g2a571b6068-linuxarm64-lgpl-shared-9.0")
    set(_ffmpeg_sha256 "1aa02c59afe06dea2602b40517a5cb75c0ff6d524f217b5aea1f0ca07977cd51")
  else()
    set(_ffmpeg_name "ffmpeg-n9.0.2-17-g2a571b6068-linux64-lgpl-shared-9.0")
    set(_ffmpeg_sha256 "7a1ba2af36e5beeb98151e766a903b1d115f2206a86571f17f873cd7292c424f")
  endif()
  if(DEFINED ENV{XDG_CACHE_HOME} AND NOT "$ENV{XDG_CACHE_HOME}" STREQUAL "")
    set(_ffmpeg_cache "$ENV{XDG_CACHE_HOME}/lychee_player")
  else()
    set(_ffmpeg_cache "$ENV{HOME}/.cache/lychee_player")
  endif()
  set(_ffmpeg_dir "${_ffmpeg_cache}/${_ffmpeg_name}")
  if(NOT EXISTS "${_ffmpeg_dir}/lib/libavcodec.so")
    set(_archive "${_ffmpeg_cache}/${_ffmpeg_name}.tar.xz")
    message(STATUS "Downloading ${_ffmpeg_name}")
    # GitHub's release CDN drops a connection now and then.
    foreach(_attempt 1 2 3)
      file(DOWNLOAD "${_ffmpeg_release}/${_ffmpeg_name}.tar.xz" "${_archive}"
        EXPECTED_HASH SHA256=${_ffmpeg_sha256} TLS_VERIFY ON STATUS _status)
      list(GET _status 0 _code)
      if(_code EQUAL 0)
        break()
      endif()
      file(REMOVE "${_archive}")
    endforeach()
    if(NOT _code EQUAL 0)
      message(FATAL_ERROR "Cannot download FFmpeg: ${_status}. "
        "Set LYCHEE_FFMPEG_ROOT to a prebuilt LGPL FFmpeg instead.")
    endif()
    set(_extract "${_ffmpeg_cache}/extract-${_ffmpeg_name}")
    file(REMOVE_RECURSE "${_extract}")
    file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_extract}")
    file(REMOVE_RECURSE "${_ffmpeg_dir}")
    file(RENAME "${_extract}/${_ffmpeg_name}" "${_ffmpeg_dir}")
    file(REMOVE_RECURSE "${_extract}")
    file(REMOVE "${_archive}")
  endif()
  # A normal variable, not cached: a deleted cache directory is downloaded
  # again on the next configure.
  set(LYCHEE_FFMPEG_ROOT "${_ffmpeg_dir}")
endif()
message(STATUS "lychee_player: FFmpeg from ${LYCHEE_FFMPEG_ROOT}")

# Copy each library under its SONAME (lib*.so.N, what the loader asks for).
# configure_file follows the symlinks.
set(_lychee_ffmpeg_bundle)
foreach(_lib avformat avcodec avutil swresample swscale)
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
add_subdirectory("${_lychee_repo}/core" "${CMAKE_CURRENT_BINARY_DIR}/core")
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
