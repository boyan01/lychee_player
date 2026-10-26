# The FFmpeg-free parsers' fuzz targets (tool/fuzz.sh): a project of its
# own, which compiles the parsers and what they use, not the library, so
# it builds on any Unix without the library's dependencies (FFmpeg,
# PulseAudio):
#
#   cmake -S core/fuzz -B build-fuzz -DLYCHEE_FUZZ_LIBFUZZER=ON
#
# With LYCHEE_FUZZ_LIBFUZZER each target links libFuzzer (clang with its
# runtime: Homebrew's LLVM, not Apple's; apt's clang); otherwise it is a
# driver that runs the inputs it is given once each.
cmake_minimum_required(VERSION 3.20)
project(lychee_fuzz LANGUAGES C CXX)

option(LYCHEE_FUZZ_LIBFUZZER "Link the fuzzers with libFuzzer" OFF)

# The top-level project's warnings, which tool/fuzz.sh also fails on.
add_compile_options(
  "$<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-Wall;-Wextra;-Wimplicit-fallthrough;-Wno-missing-field-initializers>"
  "$<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-Wnon-virtual-dtor>"
)

set(_src "${CMAKE_CURRENT_SOURCE_DIR}/../src")
add_library(lychee_parsers STATIC
  ${_src}/backends/hls/hls_demuxer.cc
  ${_src}/backends/hls/hls_playlist.cc
  ${_src}/backends/matroska/matroska_demuxer.cc
  ${_src}/backends/mp2t/elementary_stream.cc
  ${_src}/backends/mp2t/ts_demuxer.cc
  ${_src}/backends/mp2t/ts_parser.cc
  ${_src}/backends/mp2t/ts_reader.cc
  ${_src}/backends/mp4/mp4_demuxer.cc
  ${_src}/base/log.cc
  ${_src}/base/time.cc
  ${_src}/media/annexb.cc
  ${_src}/media/codec.cc
  ${_src}/media/data_source.cc
  ${_src}/media/text_subtitle_decoder.cc
)
target_include_directories(lychee_parsers PUBLIC ${_src} ../include)
target_compile_features(lychee_parsers PUBLIC cxx_std_20)
target_compile_definitions(lychee_parsers PRIVATE LYCHEE_IMPLEMENTATION=1)
find_package(Threads REQUIRED)
target_link_libraries(lychee_parsers PUBLIC Threads::Threads)
if(APPLE)
  # OpenDataSource() reaches the NSURLSession client for http(s).
  enable_language(OBJCXX)
  target_sources(lychee_parsers PRIVATE ${_src}/backends/apple/http_data_source.mm)
  set_source_files_properties(${_src}/backends/apple/http_data_source.mm
    PROPERTIES COMPILE_OPTIONS -fobjc-arc)
  target_link_libraries(lychee_parsers PUBLIC "-framework Foundation")
endif()

# lychee_TARGET_fuzzer: the directory regressions/TARGET holds its inputs.
foreach(_target mp4:FuzzMp4Demuxer matroska:FuzzMatroskaDemuxer mpegts:FuzzTsDemuxer
                hls_playlist:FuzzHlsPlaylist hls:FuzzHlsDemuxer)
  string(REPLACE ":" ";" _target "${_target}")
  list(GET _target 0 _name)
  list(GET _target 1 _function)
  set(_exe lychee_${_name}_fuzzer)
  add_executable(${_exe} fuzzer.cc)
  target_link_libraries(${_exe} PRIVATE lychee_parsers)
  target_compile_definitions(${_exe} PRIVATE LYCHEE_FUZZ_FUNCTION=${_function})
  if(LYCHEE_FUZZ_LIBFUZZER)
    target_compile_options(${_exe} PRIVATE -fsanitize=fuzzer)
    target_link_options(${_exe} PRIVATE -fsanitize=fuzzer)
  else()
    target_compile_definitions(${_exe} PRIVATE LYCHEE_FUZZ_STANDALONE=1)
  endif()
endforeach()
