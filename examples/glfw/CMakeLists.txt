# Windowed demo. macOS only for now (Metal); other platforms will add their
# own renderer next to main.mm. Skipped when GLFW is not installed.
if(NOT APPLE)
  return()
endif()

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
target_compile_features(lychee_glfw PRIVATE cxx_std_20)
target_compile_options(lychee_glfw PRIVATE -fobjc-arc)
