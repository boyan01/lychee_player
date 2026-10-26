add_executable(lychee_unittests
  main.cc
  annexb_test.cc
  apple_frame_converter_test.cc
  c_api_test.cc
  audio_renderer_algorithm_test.cc
  cpu_frame_converter_test.cc
  live_demuxer_test.cc
  live_timeline_test.cc
  media_clock_test.cc
  packet_queue_test.cc
  resampler_test.cc
  subtitle_renderer_test.cc
  text_subtitle_decoder_test.cc
  video_renderer_algorithm_test.cc
  vsync_estimator_test.cc
)
# The FFmpeg-free Matroska demuxer and text subtitle decoder are built into
# the library only without FFmpeg; the tests compile them where it lacks
# them, so the FFmpeg builds (CI) run their tests too.
get_target_property(_lychee_sources lychee SOURCES)
set(_lychee_test_sources)
foreach(_source src/backends/matroska/matroska_demuxer.cc
                src/media/text_subtitle_decoder.cc)
  if(NOT _source IN_LIST _lychee_sources)
    list(APPEND _lychee_test_sources ../${_source})
  endif()
endforeach()
# The demuxer needs OpenDataSource(), which uses fseeko (not on Windows)
# and on Apple the NSURLSession client.
if(NOT WIN32)
  target_sources(lychee_unittests PRIVATE matroska_demuxer_test.cc)
  if(NOT src/media/data_source.cc IN_LIST _lychee_sources)
    list(APPEND _lychee_test_sources ../src/media/data_source.cc)
    if(APPLE)
      list(APPEND _lychee_test_sources ../src/backends/apple/http_data_source.mm)
      set_source_files_properties(../src/backends/apple/http_data_source.mm
        PROPERTIES COMPILE_OPTIONS -fobjc-arc)
      target_link_libraries(lychee_unittests PRIVATE "-framework Foundation")
    endif()
  endif()
else()
  list(REMOVE_ITEM _lychee_test_sources ../src/backends/matroska/matroska_demuxer.cc)
endif()
target_sources(lychee_unittests PRIVATE ${_lychee_test_sources})
# Built into the library only without FFmpeg on Android.
if(LYCHEE_WITH_FFMPEG OR NOT ANDROID)
  target_sources(lychee_unittests PRIVATE ../src/media/resampler.cc)
endif()
# The tests reach into the library's internals.
target_include_directories(lychee_unittests PRIVATE ../src)
target_link_libraries(lychee_unittests PRIVATE lychee)
target_compile_features(lychee_unittests PRIVATE cxx_std_20)
add_test(NAME lychee_unittests COMMAND lychee_unittests)

# The FFmpeg-free backends, built only without FFmpeg.
if(APPLE AND NOT LYCHEE_WITH_FFMPEG)
  target_sources(lychee_unittests PRIVATE mp4_demuxer_test.cc)
endif()
