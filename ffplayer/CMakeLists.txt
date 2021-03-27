cmake_minimum_required(VERSION 3.10)

if (WIN32)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
    #    set(BUILD_SHARED_LIBS TRUE)
    set(CMAKE_SYSTEM_VERSION "10")
    set(CMAKE_SYSTEM_NAME "WindowsStore")
endif ()

if (NOT MEDIA_USE_SDL AND NOT CMAKE_FLUTTER_MEDIA)
    set(MEDIA_USE_SDL "1")
endif ()

project("media" "CXX" "C")
set(CMAKE_CXX_STANDARD 14)

if (NOT DEFINED FFP_LIBS)
    list(APPEND MEDIA_THIRD_PARTY_LIBS
            avutil
            avcodec
            avformat
            swscale
            swresample
            SDL2
            )
    if (WIN32)
        set(WINDOWS_LIB_PATH "${CMAKE_CURRENT_SOURCE_DIR}/libs/windows")

        function(add_windows_lib NAME)
            add_library(${NAME} SHARED IMPORTED GLOBAL)
            SET_PROPERTY(TARGET ${NAME} PROPERTY IMPORTED_IMPLIB ${WINDOWS_LIB_PATH}/lib/${NAME}.lib)
            if (DEFINED ARGV1)
                SET_PROPERTY(TARGET ${NAME} PROPERTY IMPORTED_LOCATION ${WINDOWS_LIB_PATH}/bin/${NAME}-${ARGV1}.dll)
            else ()
                SET_PROPERTY(TARGET ${NAME} PROPERTY IMPORTED_LOCATION ${WINDOWS_LIB_PATH}/bin/${NAME}.dll)
            endif ()
        endfunction()

        add_windows_lib(avutil 56)
        add_windows_lib(avcodec 58)
        add_windows_lib(avformat 58)
        add_windows_lib(swscale 5)
        add_windows_lib(swresample 3)
        add_windows_lib(SDL2)

    endif ()

    list(APPEND FFP_LIBS ${MEDIA_THIRD_PARTY_LIBS})
    if (UNIX)
        # linux do not have math library.
        list(APPEND FFP_LIBS m pthread)
    endif ()
endif ()


# Import Google Test: https://github.com/google/googletest/blob/master/googletest/README.md

configure_file(CMakeLists.txt.in googletest-download/CMakeLists.txt)
execute_process(COMMAND ${CMAKE_COMMAND} -G "${CMAKE_GENERATOR}" .
        RESULT_VARIABLE result
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/googletest-download )
if(result)
    message(FATAL_ERROR "CMake step for googletest failed: ${result}")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} --build .
        RESULT_VARIABLE result
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/googletest-download )
if(result)
    message(FATAL_ERROR "Build step for googletest failed: ${result}")
endif()

# Prevent overriding the parent project's compiler/linker
# settings on Windows
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

# Add googletest directly to our build. This defines
# the gtest and gtest_main targets.
add_subdirectory(${CMAKE_CURRENT_BINARY_DIR}/googletest-src
        ${CMAKE_CURRENT_BINARY_DIR}/googletest-build
        EXCLUDE_FROM_ALL)


add_subdirectory(base)

add_subdirectory(player)

if (MEDIA_USE_SDL)
    add_subdirectory(sdl)
endif ()

if (CMAKE_FLUTTER_MEDIA)
    add_subdirectory(flutter)
else ()
    add_subdirectory(example)
endif ()
