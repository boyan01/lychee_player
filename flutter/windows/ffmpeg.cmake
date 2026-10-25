# Fetches the pinned LGPL-only, shared FFmpeg for Windows x64 (BtbN
# FFmpeg-Builds, a month-end autobuild, which that project keeps) and checks
# that every library reports an LGPL license.
#
# include() it and call lychee_fetch_ffmpeg(<dir> <out_var>), or run it as
#   cmake -DDEST=<dir> -P flutter/windows/ffmpeg.cmake
# and pass the printed root to -DLYCHEE_FFMPEG_ROOT for a standalone core
# build.

set(LYCHEE_FFMPEG_NAME "ffmpeg-n9.0.2-17-g2a571b6068-win64-lgpl-shared-9.0")
set(LYCHEE_FFMPEG_URL "https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-09-30-13-08/${LYCHEE_FFMPEG_NAME}.zip")
set(LYCHEE_FFMPEG_SHA256 "7157177b8a6cb2174c1650ba8c71b363f2c78cba5330f88c4c02cf5b2b880646")
# What the core links and the app ships.
set(LYCHEE_FFMPEG_LIBRARIES avformat avcodec swresample swscale avutil)

function(lychee_fetch_ffmpeg dir out_root)
  set(root "${dir}/${LYCHEE_FFMPEG_NAME}")
  set(stamp "${root}/.lychee-sha256")
  if(EXISTS "${stamp}")
    file(READ "${stamp}" fetched)
  endif()
  set(${out_root} "${root}" PARENT_SCOPE)
  if(fetched STREQUAL LYCHEE_FFMPEG_SHA256)
    return()
  endif()

  set(archive "${dir}/${LYCHEE_FFMPEG_NAME}.zip")
  message(STATUS "Downloading ${LYCHEE_FFMPEG_URL}")
  file(DOWNLOAD "${LYCHEE_FFMPEG_URL}" "${archive}"
    EXPECTED_HASH SHA256=${LYCHEE_FFMPEG_SHA256}
    STATUS status)
  list(GET status 0 code)
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "FFmpeg download failed: ${status}")
  endif()
  file(REMOVE_RECURSE "${root}")
  file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${dir}")
  file(REMOVE "${archive}")

  # Each library embeds its license string, e.g. "libavcodec license: LGPL
  # version 3 or later"; a GPL or nonfree build says so there.
  foreach(lib ${LYCHEE_FFMPEG_LIBRARIES})
    file(GLOB dll "${root}/bin/${lib}-*.dll")
    if(NOT dll)
      message(FATAL_ERROR "FFmpeg: ${lib} DLL missing in ${root}/bin")
    endif()
    file(STRINGS "${dll}" license REGEX "^lib${lib} license: ")
    if(NOT license MATCHES "license: LGPL version")
      message(FATAL_ERROR "FFmpeg: ${dll} is not LGPL-only: '${license}'")
    endif()
  endforeach()
  file(WRITE "${stamp}" "${LYCHEE_FFMPEG_SHA256}")
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
  if(NOT DEST)
    message(FATAL_ERROR "usage: cmake -DDEST=<dir> -P ffmpeg.cmake")
  endif()
  file(MAKE_DIRECTORY "${DEST}")
  lychee_fetch_ffmpeg("${DEST}" root)
  message("${root}")
endif()
