cmake_minimum_required(VERSION 3.14)

project("lychee_player" "CXX")
set(CMAKE_CXX_STANDARD 17)

if (WIN32)
  set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
  set(CMAKE_SYSTEM_VERSION "10")
  set(CMAKE_SYSTEM_NAME "WindowsStore")

  add_compile_options("$<$<C_COMPILER_ID:MSVC>:/utf-8>")
  add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
endif ()

list(APPEND MEDIA_THIRD_PARTY_LIBS
  avutil
  avcodec
  avformat
  swscale
  swresample
  SDL2
  )

if (UNIX)
  list(APPEND MEDIA_THIRD_PARTY_LIBS m pthread)
endif ()


if (WIN32)
  set(WINDOWS_LIB_PATH "${CMAKE_CURRENT_SOURCE_DIR}/libs/windows")

  function(add_windows_lib NAME)
    add_library(${NAME} SHARED IMPORTED GLOBAL)
    SET_PROPERTY(TARGET ${NAME} PROPERTY IMPORTED_IMPLIB ${WINDOWS_LIB_PATH}/lib/${NAME}.lib)
    if (DEFINED ARGV1)
      SET_PROPERTY(TARGET ${NAME} PROPERTY IMPORTED_LOCATION ${WINDOWS_LIB_PATH}/bin/${NAME}-${ARGV1}.dll)
    else ()
      SET_PROPERTY(TARGET ${NAME} PROPERTY IMPORTED_LOCATION ${WINDOWS_LIB_PATH}/bin/${NAME}.dll)
    endif ()
  endfunction()

  add_windows_lib(avutil 56)
  add_windows_lib(avcodec 58)
  add_windows_lib(avformat 58)
  add_windows_lib(swscale 5)
  add_windows_lib(swresample 3)
  add_windows_lib(SDL2)

endif ()


add_library("lychee_player" STATIC
  lychee_player.h
  lychee_player.cc
  demuxer.cc
  demuxer.h
  ffmpeg_utils.cc
  ffmpeg_utils.h
  demuxer_stream.cc
  demuxer_stream.h
  audio_decode_config.h
  decoder_buffer_queue.cc
  decoder_buffer_queue.h
  decoder_buffer.cc
  decoder_buffer.h
  audio_decoder.cc
  audio_decoder.h
  audio_buffer.h
  audio_buffer.cc
  ffmpeg_decoding_loop.h
  ffmpeg_decoding_loop.cc
  ffmpeg_glue.h
  ffmpeg_glue.cc
  audio_device_info.h
  audio_renderer.cc
  audio_renderer.h
  sdl_audio_renderer.cc
  sdl_audio_renderer.h)

add_subdirectory(base)

target_include_directories("lychee_player" PUBLIC "third_party" ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries("lychee_player" PUBLIC ${MEDIA_THIRD_PARTY_LIBS} lychee_base)

if (LYCHEE_PLAYER_BUILD_EXAMPLE)
  message("Building example")
  add_subdirectory(example)
endif ()

add_subdirectory(flutter)

if (NOT LYCHEE_PLAYER_BUILD_SHARED_FLUTTER_PLUGIN)
  message("Building static flutter plugin")
  install(TARGETS lychee_player lychee_base lychee_player_plugin ARCHIVE DESTINATION lib)
  install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/flutter/lychee_player_plugin.h DESTINATION include)
endif ()