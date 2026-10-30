# Windowed demo, one renderer per platform: main.mm draws with Metal on
# macOS, main_win.cc with Direct3D 11 on Windows, main_linux.cc with OpenGL
# ES on EGL on Linux. Skipped elsewhere, and when GLFW (on Linux also EGL or
# GLES) is unavailable.
if(APPLE)
  find_package(glfw3 3.3 QUIET)
  if(NOT glfw3_FOUND)
    message(STATUS "GLFW not found, skipping examples/glfw (brew install glfw)")
    return()
  endif()

  add_executable(lychee_glfw main.mm)
  target_link_libraries(lychee_glfw PRIVATE
    lychee
    glfw
    "-framework AppKit"
    "-framework CoreVideo"
    "-framework Metal"
    "-framework QuartzCore"
  )
  target_compile_options(lychee_glfw PRIVATE -fobjc-arc)
elseif(WIN32)
  # Windows has no system package manager to take GLFW from, and its
  # prebuilt binaries are per compiler, so build a pinned release from
  # source (seconds). Offline, -DLYCHEE_FETCH_GLFW=OFF skips the example.
  option(LYCHEE_FETCH_GLFW "Download GLFW to build examples/glfw" ON)
  if(NOT LYCHEE_FETCH_GLFW)
    message(STATUS "LYCHEE_FETCH_GLFW is off, skipping examples/glfw")
    return()
  endif()
  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
  endif()
  include(FetchContent)
  FetchContent_Declare(glfw
    URL https://github.com/glfw/glfw/releases/download/3.5.1/glfw-3.5.1.zip
    URL_HASH SHA256=ea79bc5feffc254c87291980c2d0bce9acebb68c4983b79f961dcd2cb8a611a0
  )
  set(GLFW_BUILD_EXAMPLES OFF)
  set(GLFW_BUILD_TESTS OFF)
  set(GLFW_BUILD_DOCS OFF)
  set(GLFW_INSTALL OFF)
  set(GLFW_LIBRARY_TYPE STATIC)
  # GLFW is not held to the project's warnings.
  get_directory_property(_lychee_options COMPILE_OPTIONS)
  set_directory_properties(PROPERTIES COMPILE_OPTIONS "")
  FetchContent_MakeAvailable(glfw)
  set_directory_properties(PROPERTIES COMPILE_OPTIONS "${_lychee_options}")

  add_executable(lychee_glfw main_win.cc)
  target_link_libraries(lychee_glfw PRIVATE lychee glfw d3d11 dxgi d3dcompiler)
  target_compile_definitions(lychee_glfw PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(glfw3 3.3 QUIET)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(LYCHEE_GLFW_GL QUIET IMPORTED_TARGET egl glesv2)
  endif()
  if(NOT glfw3_FOUND OR NOT LYCHEE_GLFW_GL_FOUND)
    message(STATUS "GLFW, EGL or GLES not found, skipping examples/glfw "
                   "(apt install libglfw3-dev libegl-dev libgles-dev)")
    return()
  endif()

  add_executable(lychee_glfw main_linux.cc)
  target_link_libraries(lychee_glfw PRIVATE lychee glfw PkgConfig::LYCHEE_GLFW_GL)
else()
  return()
endif()

target_compile_features(lychee_glfw PRIVATE cxx_std_20)
