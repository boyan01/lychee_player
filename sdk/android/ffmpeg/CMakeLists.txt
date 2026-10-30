cmake_minimum_required(VERSION 3.22.1)
project(lychee_ffmpeg LANGUAGES C CXX)

get_filename_component(core_root "${CMAKE_CURRENT_SOURCE_DIR}/../../../core" ABSOLUTE)
set(ffmpeg_root "${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/ffmpeg/android-${ANDROID_ABI}")
add_library(lychee_ffmpeg SHARED
  ${core_root}/src/backends/ffmpeg/ffmpeg_extension.cc
  ${core_root}/src/backends/ffmpeg/ffmpeg_common.cc
  ${core_root}/src/backends/ffmpeg/ffmpeg_demuxer.cc
  ${core_root}/src/backends/ffmpeg/ffmpeg_video_decoder.cc
  ${core_root}/src/backends/ffmpeg/ffmpeg_audio_decoder.cc
  ${core_root}/src/backends/ffmpeg/ffmpeg_subtitle_decoder.cc
  ${core_root}/src/backends/ffmpeg/cleartext_redirect.cc)
target_compile_features(lychee_ffmpeg PRIVATE cxx_std_20)
target_include_directories(lychee_ffmpeg PRIVATE ${core_root}/src ${core_root}/include)
target_include_directories(lychee_ffmpeg SYSTEM PRIVATE "${ffmpeg_root}/include")
target_compile_options(lychee_ffmpeg PRIVATE -O2 -Wall -Wextra -Werror
  -Wimplicit-fallthrough -Wnon-virtual-dtor -Wno-missing-field-initializers)
set_target_properties(lychee_ffmpeg PROPERTIES
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_link_options(lychee_ffmpeg PRIVATE -Wl,-z,max-page-size=16384)
foreach(lib avformat avcodec swresample swscale avutil)
  add_library(ffmpeg_${lib} SHARED IMPORTED)
  set_target_properties(ffmpeg_${lib} PROPERTIES
    IMPORTED_LOCATION "${ffmpeg_root}/lib/lib${lib}.so")
  target_link_libraries(lychee_ffmpeg PRIVATE ffmpeg_${lib})
endforeach()

include(${core_root}/cmake/ffmpeg_extension.cmake)
lychee_extension_fingerprint(lychee_ffmpeg)
