cmake_minimum_required(VERSION 3.10)

project("media_player_example")

set(EXAMPLE_BIN_NAME ${PROJECT_NAME})

if (WIN32)
elseif (APPLE)
    # TODO auto download from https://github.com/JetBrains/skia-build/releases
    # https://github.com/aseprite/skia/releases
    # example: https://github.com/JetBrains/skija/blob/master/script/build.py
    set(SKIA_ARCH "x64")
    set(SKIA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/Skia-macOS-Release-${SKIA_ARCH}")
    set(SKIA_LIBRARY_DIR "${SKIA_DIR}/out/Release-x64")
elseif (LINUX)
endif ()

add_executable(${EXAMPLE_BIN_NAME}
        media_player_example.cc
        skia_media_texture.h
        skia_media_texture.cc
        app_window.h
        )
set_property(TARGET ${EXAMPLE_BIN_NAME} PROPERTY CXX_STANDARD 17)

list(APPEND EXAMPLE_LIBRARIES media_player)

if (APPLE)
    list(APPEND EXAMPLE_LIBRARIES media_macos)
endif ()


set(LAF_WITH_TESTS OFF)
set(DLAF_WITH_EXAMPLES OFF)
include(FetchContent)
FetchContent_Declare(
        laf
        GIT_REPOSITORY https://github.com/aseprite/laf.git
        GIT_TAG f58ece8248e2ebf6124b28ec49044be913f57292
)
FetchContent_MakeAvailable(laf)

target_link_libraries(${EXAMPLE_BIN_NAME} PUBLIC laf-os ${EXAMPLE_LIBRARIES})

if (WIN32)
    if (MEDIA_WINDOWS)
        set_property(TARGET ${EXAMPLE_BIN_NAME} APPEND PROPERTY LINK_FLAGS "/NODEFAULTLIB:MSVCRT")
    endif ()
    # Copy vcpkg ffmpeg libs to executable directory.
    foreach (LIB IN LISTS MEDIA_THIRD_PARTY_LIBS)
        add_custom_command(TARGET ${EXAMPLE_BIN_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:${LIB}>
                $<TARGET_FILE_DIR:${EXAMPLE_BIN_NAME}>)
    endforeach ()
endif ()

install(TARGETS ${PROJECT_NAME} DESTINATION bin)