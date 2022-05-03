cmake_minimum_required(VERSION 3.15)

project("lychee_player" "CXX")
set(CMAKE_CXX_STANDARD 17)

list(APPEND MEDIA_THIRD_PARTY_LIBS
  avutil
  avcodec
  avformat
  swscale
  swresample
  sdl2
  )


add_library("lychee_player"
  lychee_player.h
  lychee_player.cc
  demuxer.cc demuxer.h
  ffmpeg_utils.cc ffmpeg_utils.h
  demuxer_stream.cc demuxer_stream.h
  audio_decode_config.h audio_decode_config.cc
  decoder_buffer_queue.cc decoder_buffer_queue.h
  decoder_buffer.cc decoder_buffer.h
  audio_decoder.cc audio_decoder.h
  audio_buffer.h audio_buffer.cc
  ffmpeg_decoding_loop.h ffmpeg_decoding_loop.cc
  ffmpeg_glue.h ffmpeg_glue.cc
  audio_device_info.cc audio_device_info.h
  )

add_subdirectory(base)

target_include_directories("lychee_player" PUBLIC "third_party")
target_link_libraries("lychee_player" PUBLIC ${MEDIA_THIRD_PARTY_LIBS} lychee_base)

