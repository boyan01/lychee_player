cmake_minimum_required(VERSION 3.10)

if (WIN32)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
    #    set(BUILD_SHARED_LIBS TRUE)
    set(CMAKE_SYSTEM_VERSION "10")
    set(CMAKE_SYSTEM_NAME "WindowsStore")
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

if (CMAKE_FLUTTER_MEDIA)
    #    add_subdirectory(flutter)
else ()
    #    add_subdirectory(example)
endif ()

if (WIN32)
    add_compile_definitions("LYCHEE_ENABLE_SDL")
elseif (UNIX AND NOT APPLE)
    add_compile_definitions("LYCHEE_ENABLE_SDL")
elseif (MEDIA_MACOS)
    add_compile_definitions("LYCHEE_OSX")
    find_library(FFMPEG_KIT NAMES FFmpegKit HINTS ${CMAKE_CURRENT_SOURCE_DIR}/../example/macos/Pods/ffmpeg-kit-macos-full REQUIRED)
    list(APPEND FFP_LIBS ${FFmpegKit})
    list(APPEND FFP_LIBS "-framework AudioToolbox")
elseif (MEDIA_IOS)
    add_compile_definitions("LYCHEE_OSX")
    find_library(FFMPEG_KIT NAMES FFmpegKit HINTS ${CMAKE_CURRENT_SOURCE_DIR}/../example/ios/Pods/ffmpeg-kit-ios-full REQUIRED)
    list(APPEND FFP_LIBS ${FFMPEG_KIT})
    list(APPEND FFP_LIBS "-framework AudioToolbox")
endif ()


add_library("lychee_player" STATIC
        decoder_base.h
        decoder_base.cc
        ffp_define.h
        ffp_frame_queue.h
        ffp_frame_queue.cc
        ffp_msg_queue.h
        ffp_msg_queue.cc
        ffp_packet_queue.h
        ffp_packet_queue.cc
        ffp_utils.h
        ffp_utils.cc
        media_clock.h
        media_clock.cc
        render_audio_base.h
        render_audio_base.cc
        render_video_base.h
        render_video_base.cc
        render_base.h
        render_base.cc
        data_source.h
        data_source.cc
        decoder_ctx.h
        decoder_ctx.cc
        ffplayer.h
        media_player.h
        media_player.cc
        decoder_audio.h
        decoder_audio.cc
        audio_render_basic.h
        audio_render_basic.cc
        audio_render_sdl.h
        audio_render_sdl.cc
        decoder_video.h
        decoder_video.cc
        location.h
        location.cc
        logging.h
        logging.cc
        message_queue.h
        message_queue.cc
        media_msg_define.h
        audio_render_core_audio.cc
        audio_render_core_audio.h
        )

target_include_directories("lychee_player" PUBLIC "third_party" ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries("lychee_player" PUBLIC ${FFP_LIBS})

add_subdirectory(flutter)

if (NOT LYCHEE_PLAYER_BUILD_SHARED_FLUTTER_PLUGIN)
    message("Building static flutter plugin")
    install(TARGETS lychee_player lychee_player_flutter ARCHIVE DESTINATION lib)
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/flutter/lychee_player_plugin.h DESTINATION include)
endif ()


if (LYCHEE_PLAYER_BUILD_EXAMPLE)
    add_subdirectory(example)
endif ()
