cmake_minimum_required(VERSION 3.20)

option(LYCHEE_WITH_FFMPEG "Build the FFmpeg demuxer and decoders" ON)
# On in the builds of the core that ship (the Flutter plugins' and the
# Android SDK's), as Flutter's plugin template does for plugin code.
option(LYCHEE_WARNINGS_AS_ERRORS "Fail the core's build on a warning" OFF)

# The warnings the project holds its own code to. As in Chromium,
# aggregates may leave fields to their defaults, which C structs that
# start with struct_size rely on.
if(MSVC)
  # The C runtime's advice to use its _s functions is not portable.
  set(_lychee_warnings /W4 /D_CRT_SECURE_NO_WARNINGS)
else()
  set(_lychee_warnings -Wall -Wextra -Wimplicit-fallthrough
    -Wno-missing-field-initializers)
endif()
# One generator expression per flag: the list goes through unquoted
# expansions, which would split a genex at its ';'.
set(LYCHEE_WARNING_OPTIONS)
foreach(_flag ${_lychee_warnings})
  list(APPEND LYCHEE_WARNING_OPTIONS
    "$<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:${_flag}>")
endforeach()
if(NOT MSVC)
  list(APPEND LYCHEE_WARNING_OPTIONS
    "$<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-Wnon-virtual-dtor>")
endif()
if(NOT CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(LYCHEE_WARNING_OPTIONS "${LYCHEE_WARNING_OPTIONS}" PARENT_SCOPE)
endif()

set(LYCHEE_SOURCES
  src/api/c_api.cc
  src/base/log.cc
  src/base/time.cc
  src/media/annexb.cc
  src/media/codec.cc
  src/media/cpu_frame_converter.cc
  src/media/factory.cc
  src/media/null_audio_sink.cc
  src/media/subtitle_decoder.cc
  src/media/text_subtitle_decoder.cc
  src/pipeline/audio_renderer.cc
  src/pipeline/audio_renderer_algorithm.cc
  src/pipeline/live_demuxer.cc
  src/pipeline/live_timeline.cc
  src/pipeline/media_clock.cc
  src/pipeline/packet_history.cc
  src/pipeline/packet_queue.cc
  src/pipeline/player.cc
  src/pipeline/subtitle_renderer.cc
  src/pipeline/video_cadence_estimator.cc
  src/pipeline/video_renderer.cc
  src/pipeline/video_renderer_algorithm.cc
  src/pipeline/vsync_estimator.cc
)

if(LYCHEE_WITH_FFMPEG)
  list(APPEND LYCHEE_SOURCES
    src/backends/ffmpeg/ffmpeg_audio_decoder.cc
    src/backends/ffmpeg/ffmpeg_common.cc
    src/backends/ffmpeg/ffmpeg_demuxer.cc
    src/backends/ffmpeg/ffmpeg_subtitle_decoder.cc
    src/backends/ffmpeg/ffmpeg_video_decoder.cc
  )
endif()

if(ANDROID AND LYCHEE_WITH_FFMPEG)
  list(APPEND LYCHEE_SOURCES src/backends/ffmpeg/cleartext_redirect.cc)
endif()

if(ANDROID)
  list(APPEND LYCHEE_SOURCES
    src/backends/android/aes.cc
    src/backends/android/subtitle_image.cc
    src/backends/android/aaudio_sink.cc
    src/backends/android/http_data_source.cc
    src/backends/android/jvm.cc
    src/backends/android/media_codec_video_decoder.cc
    # Its SPS parser finds size changes for MediaCodec.
    src/backends/mp2t/elementary_stream.cc
    src/media/data_source.cc
    # MP4, MPEG-TS, HLS and Matroska from files and http(s). With FFmpeg
    # only MPEG-TS and HLS go to these (media/factory.cc), as on Apple.
    src/backends/hls/hls_demuxer.cc
    src/backends/hls/hls_playlist.cc
    src/backends/matroska/matroska_demuxer.cc
    src/backends/mp2t/ts_demuxer.cc
    src/backends/mp2t/ts_parser.cc
    src/backends/mp2t/ts_reader.cc
    src/backends/mp4/mp4_demuxer.cc
    src/backends/mp4/mp4_subtitle.cc
    src/media/probing_demuxer.cc
  )
  if(NOT LYCHEE_WITH_FFMPEG)
    # Without FFmpeg: H.264, HEVC, VP8, VP9 and AV1 through MediaCodec
    # (software codecs as the fallback), AAC, MP3, Opus, FLAC and ALAC
    # through MediaCodec with our own channel mix and resampler, text
    # subtitles.
    list(APPEND LYCHEE_SOURCES
      src/backends/android/media_codec_audio_decoder.cc
      src/media/resampler.cc
    )
  endif()
endif()

if(WIN32)
  # Media Foundation decodes H.264 and HEVC in both builds, ahead of FFmpeg.
  list(APPEND LYCHEE_SOURCES
    src/backends/media_foundation/mf_common.cc
    src/backends/media_foundation/mf_video_decoder.cc
    src/backends/windows/subtitle_image.cc
    src/backends/windows/d3d11_video_processor.cc
    src/backends/windows/http_data_source.cc
    src/backends/windows/wasapi_sink.cc
    # Files and http(s) (WinHTTP) for both demuxers.
    src/media/data_source.cc
    # MP4, Matroska/WebM, MPEG-TS and HLS from files and http(s). With
    # FFmpeg only MPEG-TS and HLS go to these (media/factory.cc), FFmpeg
    # the rest.
    src/backends/hls/hls_demuxer.cc
    src/backends/hls/hls_playlist.cc
    src/backends/matroska/matroska_demuxer.cc
    src/backends/mp2t/elementary_stream.cc
    src/backends/mp2t/ts_demuxer.cc
    src/backends/mp2t/ts_parser.cc
    src/backends/mp2t/ts_reader.cc
    src/backends/mp4/mp4_demuxer.cc
    src/backends/mp4/mp4_subtitle.cc
    src/media/probing_demuxer.cc
  )
  if(NOT LYCHEE_WITH_FFMPEG)
    # Without FFmpeg: H.264 and HEVC in hardware or software, AAC, MP3,
    # FLAC, ALAC and Opus through Media Foundation (rate conversion by our
    # own resampler), text subtitles.
    list(APPEND LYCHEE_SOURCES
      src/backends/media_foundation/mf_audio_decoder.cc
      src/media/resampler.cc
    )
  endif()
endif()

if(UNIX AND NOT APPLE AND NOT ANDROID)
  list(APPEND LYCHEE_SOURCES
    src/backends/linux/subtitle_image.cc
    src/backends/linux/pulse_audio_sink.cc
  )
  if(LYCHEE_WITH_FFMPEG)
    # MPEG-TS and HLS through the own demuxers (media/factory.cc), which
    # read http(s) through FFmpeg's protocols; FFmpeg the rest (MP4 and
    # Matroska are only linked: ProbingDemuxer names them).
    list(APPEND LYCHEE_SOURCES
      src/backends/ffmpeg/ffmpeg_data_source.cc
      src/backends/hls/hls_demuxer.cc
      src/backends/hls/hls_playlist.cc
      src/backends/matroska/matroska_demuxer.cc
      src/backends/mp2t/elementary_stream.cc
      src/backends/mp2t/ts_demuxer.cc
      src/backends/mp2t/ts_parser.cc
      src/backends/mp2t/ts_reader.cc
      src/backends/mp4/mp4_demuxer.cc
      src/backends/mp4/mp4_subtitle.cc
      src/media/data_source.cc
      src/media/probing_demuxer.cc
    )
  endif()
endif()

if(APPLE)
  list(APPEND LYCHEE_SOURCES
    src/backends/apple/subtitle_image.cc
    src/backends/apple/apple_frame_converter.cc
    src/backends/apple/core_audio_sink.cc
    src/backends/apple/http_data_source.mm
    src/backends/apple/pixel_buffer.cc
    src/backends/apple/vt_video_decoder.cc
    # Files and http(s) for both demuxers.
    src/media/data_source.cc
    # MP4 (also fragmented), MPEG-TS and HLS from files and http(s), and
    # Matroska and WebM. With FFmpeg only MPEG-TS and HLS (with fMP4
    # segments) go to these (media/factory.cc), FFmpeg the rest.
    src/backends/hls/hls_demuxer.cc
    src/backends/hls/hls_playlist.cc
    src/backends/matroska/matroska_demuxer.cc
    src/backends/mp2t/elementary_stream.cc
    src/backends/mp2t/ts_demuxer.cc
    src/backends/mp2t/ts_parser.cc
    src/backends/mp2t/ts_reader.cc
    src/backends/mp4/mp4_demuxer.cc
    src/backends/mp4/mp4_subtitle.cc
    src/media/probing_demuxer.cc
  )
  if(NOT LYCHEE_WITH_FFMPEG)
    # Without FFmpeg: AAC, ALAC, Opus, FLAC and MP3 through AudioToolbox;
    # tx3g and text subtitles.
    list(APPEND LYCHEE_SOURCES
      src/backends/apple/at_audio_decoder.cc
    )
  endif()
  if(IOS)
    list(APPEND LYCHEE_SOURCES src/backends/apple/audio_session.mm)
  endif()
endif()

add_library(lychee ${LYCHEE_SOURCES})
target_compile_features(lychee PUBLIC cxx_std_20)
target_include_directories(lychee
  PUBLIC include
  PRIVATE src
)
set_target_properties(lychee PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  OBJCXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)
target_compile_definitions(lychee PRIVATE LYCHEE_IMPLEMENTATION=1)
target_compile_options(lychee PRIVATE ${LYCHEE_WARNING_OPTIONS})
if(LYCHEE_WARNINGS_AS_ERRORS)
  # Not COMPILE_WARNING_AS_ERROR: Android's and Ubuntu 22.04's CMake
  # predate it.
  if(MSVC)
    target_compile_options(lychee PRIVATE /WX)
  else()
    target_compile_options(lychee PRIVATE -Werror)
  endif()
endif()
if(BUILD_SHARED_LIBS)
  target_compile_definitions(lychee PUBLIC LYCHEE_SHARED=1)
endif()

# A prebuilt FFmpeg (tool/build_ffmpeg.sh output: include/ and lib/), or
# the system's through pkg-config.
set(LYCHEE_FFMPEG_ROOT "" CACHE PATH "Prebuilt FFmpeg to use instead of pkg-config")

if(LYCHEE_WITH_FFMPEG)
  if(LYCHEE_FFMPEG_ROOT)
    add_library(lychee_ffmpeg INTERFACE)
    # SYSTEM: FFmpeg's headers are not held to the project's warnings.
    target_include_directories(lychee_ffmpeg SYSTEM INTERFACE
      "${LYCHEE_FFMPEG_ROOT}/include")
    # find_library() keeps what it found in the cache: after a change of
    # root it would link the old root's libraries with the new headers.
    if(NOT LYCHEE_FFMPEG_ROOT STREQUAL "${LYCHEE_FFMPEG_FOUND_ROOT}")
      foreach(lib avformat avcodec swresample swscale avutil)
        unset(LYCHEE_FFMPEG_${lib} CACHE)
      endforeach()
      set(LYCHEE_FFMPEG_FOUND_ROOT "${LYCHEE_FFMPEG_ROOT}" CACHE INTERNAL "")
    endif()
    foreach(lib avformat avcodec swresample swscale avutil)
      find_library(LYCHEE_FFMPEG_${lib} ${lib} PATHS "${LYCHEE_FFMPEG_ROOT}/lib"
        NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
      # IMPORTED, so the Android Gradle plugin packages the libraries.
      add_library(lychee_ffmpeg_${lib} SHARED IMPORTED)
      set_target_properties(lychee_ffmpeg_${lib} PROPERTIES
        IMPORTED_LOCATION "${LYCHEE_FFMPEG_${lib}}")
      target_link_libraries(lychee_ffmpeg INTERFACE lychee_ffmpeg_${lib})
    endforeach()
    target_link_libraries(lychee PRIVATE lychee_ffmpeg)
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
      libavformat libavcodec libavutil libswresample libswscale)
    target_link_libraries(lychee PRIVATE PkgConfig::FFMPEG)
  endif()
  target_compile_definitions(lychee PRIVATE LYCHEE_WITH_FFMPEG=1)
endif()

if(APPLE)
  target_link_libraries(lychee PRIVATE
    "-framework ImageIO"
    "-framework CoreGraphics"
    "-framework Accelerate"
    "-framework AudioToolbox"
    "-framework CoreAudio"
    "-framework CoreFoundation"
    "-framework CoreMedia"
    "-framework CoreVideo"
    "-framework IOSurface"
    "-framework VideoToolbox"
  )
  if(IOS)
    set_source_files_properties(src/backends/apple/audio_session.mm
      PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    target_link_libraries(lychee PRIVATE "-framework AVFAudio")
  endif()
  set_source_files_properties(src/backends/apple/http_data_source.mm
    PROPERTIES COMPILE_OPTIONS -fobjc-arc)
  target_link_libraries(lychee PRIVATE "-framework Foundation")
endif()

if(ANDROID)
  target_link_libraries(lychee PRIVATE aaudio mediandk nativewindow log jnigraphics dl)
endif()

if(WIN32)
  target_link_libraries(lychee PRIVATE
    windowscodecs bcrypt ole32 avrt d3d11 d3dcompiler mfplat mfuuid winhttp wmcodecdspuuid)
  # base/thread_name.h includes <windows.h>, whose min/max macros break
  # std::min and numeric_limits::max in the files that include it.
  target_compile_definitions(lychee PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    # Sources and FFmpeg's headers are UTF-8, not the system code page.
    target_compile_options(lychee PRIVATE /utf-8)
  endif()
  if(LYCHEE_WITH_FFMPEG AND LYCHEE_FFMPEG_ROOT)
    # On Windows find_library finds the import libraries (avcodec.lib); a
    # SHARED IMPORTED target links its IMPORTED_IMPLIB and locates the DLL
    # (bin/avcodec-63.dll) through IMPORTED_LOCATION.
    foreach(lib avformat avcodec swresample swscale avutil)
      file(GLOB _lychee_dll "${LYCHEE_FFMPEG_ROOT}/bin/${lib}-*.dll")
      set_target_properties(lychee_ffmpeg_${lib} PROPERTIES
        IMPORTED_IMPLIB "${LYCHEE_FFMPEG_${lib}}")
      if(_lychee_dll)
        set_target_properties(lychee_ffmpeg_${lib} PROPERTIES
          IMPORTED_LOCATION "${_lychee_dll}")
      endif()
    endforeach()
  endif()
endif()

if(UNIX AND NOT APPLE AND NOT ANDROID)
  find_package(PkgConfig REQUIRED)
  find_package(PNG REQUIRED)
  target_link_libraries(lychee PRIVATE PNG::PNG)
  pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse)
  target_link_libraries(lychee PRIVATE PkgConfig::PULSE)
endif()

find_package(Threads REQUIRED)
target_link_libraries(lychee PRIVATE Threads::Threads)
