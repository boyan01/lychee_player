# Fetches the LGPL-only, shared FFmpeg for Linux and checks that every
# library reports an LGPL license. It is this project's own build
# (tool/build_ffmpeg.sh linux-x86_64 or linux-arm64, LGPL 2.1 with GnuTLS),
# the asset ffmpeg-linux-ARCH.tar.xz of the GitHub release that
# ../prebuilt.json pins, with its SHA-256; LYCHEE_PREBUILT_URL names a
# mirror of the release URL, as for the other platforms. While
# prebuilt.json pins no such asset (1.0.0-beta.1 has none; tool/release.sh
# pins both from the next release on), the plugin takes BtbN's
# FFmpeg-Builds instead (LGPL 3), pinned below. That one aborts at the
# first VAAPI frame on libva older than 2.21 (docs/roadmap.md): delete the
# fallback once a release carries ours.
#
# include() it and call lychee_fetch_ffmpeg(<dir> <out_var>); the result has
# include/ and lib/ with the shared libraries.

set(LYCHEE_FFMPEG_BTBN_RELEASE
  "https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-09-30-13-08")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  set(LYCHEE_FFMPEG_ASSET "ffmpeg-linux-arm64")
  set(LYCHEE_FFMPEG_BTBN_NAME "ffmpeg-n9.0.2-17-g2a571b6068-linuxarm64-lgpl-shared-9.0")
  set(LYCHEE_FFMPEG_BTBN_SHA256 "1aa02c59afe06dea2602b40517a5cb75c0ff6d524f217b5aea1f0ca07977cd51")
else()
  set(LYCHEE_FFMPEG_ASSET "ffmpeg-linux-x86_64")
  set(LYCHEE_FFMPEG_BTBN_NAME "ffmpeg-n9.0.2-17-g2a571b6068-linux64-lgpl-shared-9.0")
  set(LYCHEE_FFMPEG_BTBN_SHA256 "7a1ba2af36e5beeb98151e766a903b1d115f2206a86571f17f873cd7292c424f")
endif()
# What the core links and the app ships.
set(LYCHEE_FFMPEG_LIBRARIES avformat avcodec swresample swscale avutil)

function(lychee_fetch_ffmpeg dir out_root)
  # The release's asset if prebuilt.json pins one. The archive holds one
  # directory of its own name, as BtbN's does; ours is kept per tag, since
  # the cache directory is shared by every app.
  file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../prebuilt.json" manifest)
  string(JSON sha256 ERROR_VARIABLE missing
    GET "${manifest}" files "${LYCHEE_FFMPEG_ASSET}.tar.xz")
  if(missing)
    set(name "${LYCHEE_FFMPEG_BTBN_NAME}")
    set(url "${LYCHEE_FFMPEG_BTBN_RELEASE}/${name}.tar.xz")
    set(sha256 "${LYCHEE_FFMPEG_BTBN_SHA256}")
    set(root "${dir}/${name}")
  else()
    set(name "${LYCHEE_FFMPEG_ASSET}")
    string(JSON release GET "${manifest}" url)
    if(NOT "$ENV{LYCHEE_PREBUILT_URL}" STREQUAL "")
      set(release "$ENV{LYCHEE_PREBUILT_URL}")
    endif()
    string(JSON tag GET "${manifest}" tag)
    set(url "${release}/${tag}/${name}.tar.xz")
    set(root "${dir}/${name}-${tag}")
  endif()

  set(stamp "${root}/.lychee-sha256")
  if(EXISTS "${stamp}")
    file(READ "${stamp}" fetched)
  endif()
  set(${out_root} "${root}" PARENT_SCOPE)
  if(fetched STREQUAL sha256)
    return()
  endif()

  set(archive "${dir}/${name}.tar.xz")
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
  set(extract "${dir}/extract-${name}")
  file(REMOVE_RECURSE "${extract}" "${root}")
  file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${extract}")
  file(RENAME "${extract}/${name}" "${root}")
  file(REMOVE_RECURSE "${extract}")
  file(REMOVE "${archive}")

  # Each library embeds its license string, e.g. "libavcodec license: LGPL
  # version 2.1 or later"; a GPL or nonfree build says so there.
  foreach(lib ${LYCHEE_FFMPEG_LIBRARIES})
    file(GLOB so "${root}/lib/lib${lib}.so.[0-9]*")
    if(NOT so)
      message(FATAL_ERROR "FFmpeg: lib${lib}.so missing in ${root}/lib")
    endif()
    list(GET so 0 so)
    file(STRINGS "${so}" license REGEX "^lib${lib} license: ")
    if(NOT license MATCHES "license: LGPL version")
      message(FATAL_ERROR "FFmpeg: ${so} is not LGPL-only: '${license}'")
    endif()
  endforeach()
  file(WRITE "${stamp}" "${sha256}")
endfunction()
