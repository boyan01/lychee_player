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

    list(APPEND FFP_LIBS ${MEDIA_THIRD_PARTY_LIBS})
    if (UNIX)
        # linux do not have math library.
        list(APPEND FFP_LIBS m pthread)
    endif ()
endif ()


if (CMAKE_FLUTTER_MEDIA_WINDOWS)
    set(CMAKE_FLUTTER_MEDIA "1")
    set(DISABLE_MEDIA_TEST 1)
    add_compile_definitions(_FLUTTER_MEDIA_)
    add_compile_definitions(_FLUTTER_MEDIA_WINDOWS)
elseif (CMAKE_FLUTTER_MEDIA_LINUX)
    set(CMAKE_FLUTTER_MEDIA "1")
    add_compile_definitions(_FLUTTER_MEDIA_)
    add_compile_definitions(_FLUTTER_MEDIA_LINUX)
elseif (CMAKE_FLUTTER_MEDIA_ANDROID)
    set(CMAKE_FLUTTER_MEDIA "1")
    set(DISABLE_MEDIA_TEST 1)
    add_compile_definitions(_FLUTTER_MEDIA_)
    add_compile_definitions(_FLUTTER_MEDIA_ANDROID)
elseif (CMAKE_FLUTTER_MEDIA_MACOS)
    set(MEDIA_USE_SDL 0)
    set(CMAKE_FLUTTER_MEDIA "1")
    set(DISABLE_MEDIA_TEST 1)
    add_compile_definitions(_FLUTTER_MEDIA_MACOS)
endif ()

if (NOT DISABLE_MEDIA_TEST)
    # https://google.github.io/googletest/quickstart-cmake.html#set-up-a-project
    include(FetchContent)
    FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest
            GIT_TAG 609281088cfefc76f9d0ce82e1ff6c30cc3591e5
    )
    # For Windows: Prevent overriding the parent project's compiler/linker settings
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif ()


add_subdirectory(base)

add_subdirectory(player)

if (MEDIA_USE_SDL)
    add_subdirectory(sdl)
endif ()

if (APPLE)
    add_subdirectory(macos)
endif ()

if (CMAKE_FLUTTER_MEDIA)
    add_subdirectory(flutter)
else ()
    add_subdirectory(example)
endif ()
