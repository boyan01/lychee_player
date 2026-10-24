cmake_minimum_required(VERSION 3.20)

option(LYCHEE_WITH_FFMPEG "Build the FFmpeg demuxer and decoders" ON)

set(LYCHEE_SOURCES
  src/api/c_api.cc
  src/base/log.cc
  src/base/time.cc
  src/media/codec.cc
  src/media/factory.cc
  src/pipeline/audio_renderer.cc
  src/pipeline/media_clock.cc
  src/pipeline/packet_queue.cc
  src/pipeline/player.cc
  src/pipeline/video_cadence_estimator.cc
  src/pipeline/video_renderer.cc
  src/pipeline/video_renderer_algorithm.cc
)

if(LYCHEE_WITH_FFMPEG)
  list(APPEND LYCHEE_SOURCES
    src/backends/ffmpeg/ffmpeg_audio_decoder.cc
    src/backends/ffmpeg/ffmpeg_common.cc
    src/backends/ffmpeg/ffmpeg_demuxer.cc
    src/backends/ffmpeg/ffmpeg_video_decoder.cc
  )
endif()

if(APPLE)
  list(APPEND LYCHEE_SOURCES
    src/backends/apple/apple_frame_converter.cc
    src/backends/apple/core_audio_sink.cc
    src/backends/apple/pixel_buffer.cc
  )
endif()

add_library(lychee ${LYCHEE_SOURCES})
target_compile_features(lychee PUBLIC cxx_std_20)
target_include_directories(lychee
  PUBLIC include
  PRIVATE src
)
set_target_properties(lychee PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)
target_compile_definitions(lychee PRIVATE LYCHEE_IMPLEMENTATION=1)
if(BUILD_SHARED_LIBS)
  target_compile_definitions(lychee PUBLIC LYCHEE_SHARED=1)
endif()

if(LYCHEE_WITH_FFMPEG)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat libavcodec libavutil libswresample libswscale)
  target_link_libraries(lychee PRIVATE PkgConfig::FFMPEG)
  target_compile_definitions(lychee PRIVATE LYCHEE_WITH_FFMPEG=1)
endif()

if(APPLE)
  target_link_libraries(lychee PRIVATE
    "-framework AudioToolbox"
    "-framework CoreAudio"
    "-framework CoreFoundation"
    "-framework CoreMedia"
    "-framework CoreVideo"
    "-framework IOSurface"
    "-framework VideoToolbox"
  )
endif()

find_package(Threads REQUIRED)
target_link_libraries(lychee PRIVATE Threads::Threads)
