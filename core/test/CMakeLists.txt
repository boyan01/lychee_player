add_executable(lychee_unittests
  main.cc
  annexb_test.cc
  audio_renderer_algorithm_test.cc
  cpu_frame_converter_test.cc
  media_clock_test.cc
  packet_queue_test.cc
  subtitle_renderer_test.cc
  video_renderer_algorithm_test.cc
  vsync_estimator_test.cc
)
# The tests reach into the library's internals.
target_include_directories(lychee_unittests PRIVATE ../src)
target_link_libraries(lychee_unittests PRIVATE lychee)
target_compile_features(lychee_unittests PRIVATE cxx_std_20)
add_test(NAME lychee_unittests COMMAND lychee_unittests)

