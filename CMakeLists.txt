cmake_minimum_required(VERSION 3.20)
project(lychee LANGUAGES C CXX)

if(APPLE)
  enable_language(OBJCXX)
endif()

# The warnings tool/regression.sh's zero-warning check holds this
# project's own code to (FFmpeg, libass and GLFW build elsewhere). As in
# Chromium, aggregates may leave fields to their defaults, which C structs
# that start with struct_size rely on. MSVC keeps its default level for
# now (roadmap: /W4).
if(NOT MSVC)
  add_compile_options(
    "$<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-Wall;-Wextra;-Wimplicit-fallthrough;-Wno-missing-field-initializers>"
    "$<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-Wnon-virtual-dtor>"
  )
endif()

option(LYCHEE_BUILD_EXAMPLES "Build the example programs" ON)
# The tests reach into the library's internals, which a shared library
# does not export.
if(BUILD_SHARED_LIBS)
  option(LYCHEE_BUILD_TESTS "Build the unit tests (static builds only)" OFF)
  if(LYCHEE_BUILD_TESTS)
    message(FATAL_ERROR "LYCHEE_BUILD_TESTS needs a static build (BUILD_SHARED_LIBS=OFF)")
  endif()
else()
  option(LYCHEE_BUILD_TESTS "Build the unit tests (static builds only)" ON)
endif()

add_subdirectory(core)

# The fuzzers cover the FFmpeg-free parsers (tool/fuzz.sh).
option(LYCHEE_BUILD_FUZZERS "Build the fuzz targets (Apple, without FFmpeg)" OFF)
if(LYCHEE_BUILD_FUZZERS)
  if(NOT APPLE OR LYCHEE_WITH_FFMPEG)
    message(FATAL_ERROR "LYCHEE_BUILD_FUZZERS needs an Apple build with LYCHEE_WITH_FFMPEG=OFF")
  endif()
  add_subdirectory(core/fuzz)
endif()

if(LYCHEE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(core/test)
endif()

if(LYCHEE_BUILD_EXAMPLES)
  add_subdirectory(examples/cli)
  add_subdirectory(examples/glfw)
endif()
