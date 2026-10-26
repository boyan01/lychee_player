# Fetches the LGPL-only, shared FFmpeg for Windows x64 and checks that every
# library reports an LGPL license. It is this project's own build
# (tool/build_ffmpeg.sh windows-x86_64), the asset ffmpeg-windows-x86_64.zip
# of the GitHub release that ../prebuilt.json pins, with its SHA-256;
# LYCHEE_PREBUILT_URL names a mirror of the release URL, as for the other
# platforms. While prebuilt.json pins no such asset (1.0.0-beta.1 has
# none; tool/release.sh pins it from the next release on), the plugin takes
# BtbN's FFmpeg-Builds instead, pinned below: delete that fallback once a
# release carries ours.
#
# include() it and call lychee_fetch_ffmpeg(<dir> <out_var>), or run it as
#   cmake -DDEST=<dir> -P flutter/windows/ffmpeg.cmake
# and pass the printed root to -DLYCHEE_FFMPEG_ROOT for a standalone core
# build.

set(LYCHEE_FFMPEG_ASSET "ffmpeg-windows-x86_64")
set(LYCHEE_FFMPEG_BTBN_NAME "ffmpeg-n9.0.2-17-g2a571b6068-win64-lgpl-shared-9.0")
set(LYCHEE_FFMPEG_BTBN_URL "https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-09-30-13-08/${LYCHEE_FFMPEG_BTBN_NAME}.zip")
set(LYCHEE_FFMPEG_BTBN_SHA256 "7157177b8a6cb2174c1650ba8c71b363f2c78cba5330f88c4c02cf5b2b880646")
# What the core links and the app ships.
set(LYCHEE_FFMPEG_LIBRARIES avformat avcodec swresample swscale avutil)

function(lychee_fetch_ffmpeg dir out_root)
  # The release's asset if prebuilt.json pins one. The zip holds one
  # directory of its own name, as BtbN's does.
  file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../prebuilt.json" manifest)
  string(JSON sha256 ERROR_VARIABLE missing
    GET "${manifest}" files "${LYCHEE_FFMPEG_ASSET}.zip")
  if(missing)
    set(name "${LYCHEE_FFMPEG_BTBN_NAME}")
    set(url "${LYCHEE_FFMPEG_BTBN_URL}")
    set(sha256 "${LYCHEE_FFMPEG_BTBN_SHA256}")
  else()
    set(name "${LYCHEE_FFMPEG_ASSET}")
    string(JSON release GET "${manifest}" url)
    if(NOT "$ENV{LYCHEE_PREBUILT_URL}" STREQUAL "")
      set(release "$ENV{LYCHEE_PREBUILT_URL}")
    endif()
    string(JSON tag GET "${manifest}" tag)
    set(url "${release}/${tag}/${name}.zip")
  endif()

  set(root "${dir}/${name}")
  set(stamp "${root}/.lychee-sha256")
  if(EXISTS "${stamp}")
    file(READ "${stamp}" fetched)
  endif()
  set(${out_root} "${root}" PARENT_SCOPE)
  if(fetched STREQUAL sha256)
    return()
  endif()

  set(archive "${dir}/${name}.zip")
  message(STATUS "Downloading ${url}")
  # GitHub's release CDN drops a connection now and then.
  foreach(attempt 1 2 3)
    file(DOWNLOAD "${url}" "${archive}"
      EXPECTED_HASH SHA256=${sha256}
      STATUS status)
    list(GET status 0 code)
    if(code EQUAL 0)
      break()
    endif()
    file(REMOVE "${archive}")
  endforeach()
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "FFmpeg download failed: ${status}. "
      "LYCHEE_PREBUILT_URL can name a mirror; LYCHEE_FFMPEG_ROOT a local FFmpeg.")
  endif()
  file(REMOVE_RECURSE "${root}")
  file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${dir}")
  file(REMOVE "${archive}")

  # Each library embeds its license string, e.g. "libavcodec license: LGPL
  # version 2.1 or later"; a GPL or nonfree build says so there.
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
  file(WRITE "${stamp}" "${sha256}")
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
  if(NOT DEST)
    message(FATAL_ERROR "usage: cmake -DDEST=<dir> -P ffmpeg.cmake")
  endif()
  file(MAKE_DIRECTORY "${DEST}")
  lychee_fetch_ffmpeg("${DEST}" root)
  message("${root}")
endif()
