# Both Android AARs use this private fingerprint before sharing C++ objects.
# It is deliberately stricter than the public C ABI: rebuilding one half
# after changing an internal header or the NDK must fail rather than crash.
function(lychee_extension_fingerprint target)
  get_filename_component(core_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
  file(GLOB_RECURSE inputs CONFIGURE_DEPENDS
    "${core_root}/src/*.h" "${core_root}/src/*.cc" "${core_root}/include/*.h"
    "${core_root}/cmake/*.cmake")
  list(APPEND inputs "${core_root}/CMakeLists.txt")
  set(LYCHEE_SDK_VERSION "" CACHE STRING "Version of the paired Android AARs")
  set(fingerprint "${LYCHEE_SDK_VERSION};${CMAKE_CXX_COMPILER_ID};${CMAKE_CXX_COMPILER_VERSION};${CMAKE_ANDROID_NDK_VERSION};${ANDROID_ABI};${ANDROID_STL};${CMAKE_CXX_FLAGS}")
  foreach(input IN LISTS inputs)
    file(SHA256 "${input}" digest)
    string(APPEND fingerprint ";${digest}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${input}")
  endforeach()
  string(SHA256 fingerprint "${fingerprint}")
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/extension")
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/extension/ffmpeg_extension_build.h"
    "#define LYCHEE_FFMPEG_EXTENSION_BUILD \"${fingerprint}\"\n")
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/extension")
endfunction()
