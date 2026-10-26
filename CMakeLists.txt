cmake_minimum_required(VERSION 3.20)
project(lychee LANGUAGES C CXX)

if(APPLE)
  enable_language(OBJCXX)
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

if(LYCHEE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(core/test)
endif()

if(LYCHEE_BUILD_EXAMPLES)
  add_subdirectory(examples/cli)
  add_subdirectory(examples/glfw)
endif()
