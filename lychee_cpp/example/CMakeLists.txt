cmake_minimum_required(VERSION 3.15)

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