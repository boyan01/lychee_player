cmake_minimum_required(VERSION 3.20)
project(lychee LANGUAGES C CXX)

if(APPLE)
  enable_language(OBJCXX)
endif()

option(LYCHEE_BUILD_EXAMPLES "Build the example programs" ON)

add_subdirectory(core)

if(LYCHEE_BUILD_EXAMPLES)
  add_subdirectory(examples/cli)
endif()
