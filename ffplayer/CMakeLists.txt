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

    find_library(LIBAVUTIL avutil REQUIRED)
    find_library(LIBAVCODEC avcodec REQUIRED)
    find_library(LIBAVFORMAT avformat REQUIRED)
    find_library(LIBSWSCALE swscale REQUIRED)
    find_library(LIBSWRESAMPLE swresample REQUIRED)

    find_library(LIBSDL2 SDL2 REQUIRED)

    list(APPEND FFP_LIBS ${MEDIA_THIRD_PARTY_LIBS})
    if (UNIX)
        # linux do not have math library.
        list(APPEND FFP_LIBS m pthread)
    endif ()
endif ()

add_subdirectory(base)
add_subdirectory(audio)
add_subdirectory(video)

if (MEDIA_USE_SDL)
    add_subdirectory(sdl)
endif ()

add_subdirectory(player)

if (CMAKE_FLUTTER_MEDIA)
    add_subdirectory(flutter)
else ()
    add_subdirectory(example)
endif ()
