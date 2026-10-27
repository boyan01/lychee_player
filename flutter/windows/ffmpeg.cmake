# Fetches the LGPL-only, shared FFmpeg for Windows x64 and checks that every
# library reports an LGPL license. It is this project's own build
# (tool/build_ffmpeg.sh windows-x86_64), the asset ffmpeg-windows-x86_64.zip
# of the GitHub release that ../prebuilt.json pins, with its SHA-256;
# LYCHEE_PREBUILT_URL names a mirror of the release URL, as for the other
# platforms.
#
# include() it and call lychee_fetch_ffmpeg(<dir> <out_var>), or run it as
#   cmake -DDEST=<dir> -P flutter/windows/ffmpeg.cmake
# and pass the printed root to -DLYCHEE_FFMPEG_ROOT for a standalone core
# build.

set(LYCHEE_FFMPEG_ASSET "ffmpeg-windows-x86_64")
# What the core links and the app ships.
set(LYCHEE_FFMPEG_LIBRARIES avformat avcodec swresample swscale avutil)

function(lychee_fetch_ffmpeg dir out_root)
  # The zip holds one directory of its own name.
  file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../prebuilt.json" manifest)
  set(name "${LYCHEE_FFMPEG_ASSET}")
  string(JSON sha256 GET "${manifest}" files "${name}.zip")
  string(JSON release GET "${manifest}" url)
  if(NOT "$ENV{LYCHEE_PREBUILT_URL}" STREQUAL "")
    set(release "$ENV{LYCHEE_PREBUILT_URL}")
  endif()
  string(JSON tag GET "${manifest}" tag)
  set(url "${release}/${tag}/${name}.zip")

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
