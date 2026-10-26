# The MP4 parser's fuzzer (tool/fuzz.sh). With LYCHEE_FUZZ_LIBFUZZER it
# links libFuzzer (clang with its runtime: Homebrew's LLVM, not Apple's);
# otherwise it is a driver that runs the inputs it is given once each.
option(LYCHEE_FUZZ_LIBFUZZER "Link the fuzzers with libFuzzer" OFF)

add_executable(lychee_mp4_fuzzer mp4_demuxer_fuzzer.cc)
target_include_directories(lychee_mp4_fuzzer PRIVATE ../src)
target_link_libraries(lychee_mp4_fuzzer PRIVATE lychee)
target_compile_features(lychee_mp4_fuzzer PRIVATE cxx_std_20)
if(LYCHEE_FUZZ_LIBFUZZER)
  target_compile_options(lychee_mp4_fuzzer PRIVATE -fsanitize=fuzzer)
  target_link_options(lychee_mp4_fuzzer PRIVATE -fsanitize=fuzzer)
else()
  target_compile_definitions(lychee_mp4_fuzzer PRIVATE LYCHEE_FUZZ_STANDALONE=1)
endif()
