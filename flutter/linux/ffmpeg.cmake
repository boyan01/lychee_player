# Fetches the LGPL-only, shared FFmpeg for Linux and checks that every
# library reports an LGPL license. It is this project's own build
# (tool/build_ffmpeg.sh linux-x86_64 or linux-arm64, LGPL 2.1 with GnuTLS),
# the asset ffmpeg-linux-ARCH.tar.xz of the GitHub release that
# ../prebuilt.json pins, with its SHA-256; LYCHEE_PREBUILT_URL names a
# mirror of the release URL, as for the other platforms.
#
# include() it and call lychee_fetch_ffmpeg(<dir> <out_var>); the result has
# include/ and lib/ with the shared libraries.

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  set(LYCHEE_FFMPEG_ASSET "ffmpeg-linux-arm64")
else()
  set(LYCHEE_FFMPEG_ASSET "ffmpeg-linux-x86_64")
endif()
# What the core links and the app ships.
set(LYCHEE_FFMPEG_LIBRARIES avformat avcodec swresample swscale avutil)

function(lychee_fetch_ffmpeg dir out_root)
  # The archive holds one directory of its own name, kept per tag, since
  # the cache directory is shared by every app.
  file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../prebuilt.json" manifest)
  set(name "${LYCHEE_FFMPEG_ASSET}")
  string(JSON sha256 GET "${manifest}" files "${name}.tar.xz")
  string(JSON release GET "${manifest}" url)
  if(NOT "$ENV{LYCHEE_PREBUILT_URL}" STREQUAL "")
    set(release "$ENV{LYCHEE_PREBUILT_URL}")
  endif()
  string(JSON tag GET "${manifest}" tag)
  set(url "${release}/${tag}/${name}.tar.xz")
  set(root "${dir}/${name}-${tag}")

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
