cmake_minimum_required(VERSION 3.14)

project("lychee_player_cpp_example" "CXX")

add_executable("lychee_player_cpp_example" main.cc)

if (APPLE)
    function(add_macos_lib NAME)
        add_library(${NAME} SHARED IMPORTED GLOBAL)
        set_target_properties(${NAME} PROPERTIES IMPORTED_LOCATION /usr/local/lib/lib${NAME}.dylib)
    endfunction()

    add_macos_lib(avutil)
    add_macos_lib(avcodec)
    add_macos_lib(avformat)
    add_macos_lib(swscale)
    add_macos_lib(swresample)
    add_macos_lib(SDL2)
endif ()

target_link_libraries(lychee_player_cpp_example PUBLIC "lychee_player")
target_include_directories(lychee_player_cpp_example PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if (WIN32)
    # Copy vcpkg ffmpeg libs to executable directory.
    foreach (LIB IN LISTS MEDIA_THIRD_PARTY_LIBS)
        add_custom_command(TARGET "lychee_player_cpp_example" POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:${LIB}>
          $<TARGET_FILE_DIR:lychee_player_cpp_example>)
    endforeach ()
endif ()
