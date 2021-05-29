cmake_minimum_required(VERSION 3.10)

project("media_player_example")

set(EXAMPLE_BIN_NAME ${PROJECT_NAME})

aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR} EXAMPLE_SOURCES)

if (WIN32)
    set(EXAMPLE_WINDOWS_LIB_PATH "${CMAKE_CURRENT_SOURCE_DIR}/libs")
    add_library(SDL2main STATIC IMPORTED)
    set_target_properties(SDL2main PROPERTIES
            IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "C"
            IMPORTED_LOCATION "${EXAMPLE_WINDOWS_LIB_PATH}/SDL2main.lib")
endif ()

if (APPLE)
    add_library(SDL2main STATIC IMPORTED)
    set_target_properties(SDL2main PROPERTIES
            IMPORTED_LOCATION "/usr/local/lib/libSDL2main.a")
endif ()

if (LINUX)
    # TODO import libs
#    add_library(SDL2main STATIC IMPORTED)
#    set_target_properties(SDL2main PROPERTIES
#            IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/libs/linux/libSDL2main.a")
endif ()

add_executable(${EXAMPLE_BIN_NAME} ${EXAMPLE_SOURCES})

target_include_directories(${EXAMPLE_BIN_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})


list(APPEND EXAMPLE_LIBRARIES media_player media_sdl SDL2main)

if (APPLE)
    list(APPEND EXAMPLE_LIBRARIES media_macos)
endif ()

target_link_libraries(${EXAMPLE_BIN_NAME} PUBLIC ${EXAMPLE_LIBRARIES})

if (WIN32)
    if (CMAKE_FLUTTER_WINDOWS)
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