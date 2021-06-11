cmake_minimum_required(VERSION 3.10)

project("media_flutter")

list(APPEND media_flutter_libs media_player)

if (CMAKE_FLUTTER_MEDIA_WINDOWS)
    list(APPEND media_flutter_libs)
elseif (CMAKE_FLUTTER_MEDIA_LINUX)
    list(APPEND media_flutter_libs flutter)
elseif (CMAKE_FLUTTER_MEDIA_MACOS OR CMAKE_FLUTTER_MEDIA_IOS)
    list(APPEND media_flutter_libs media_macos)
    set(MEDIA_BUILD_STATIC 1)
endif ()

if (CMAKE_FLUTTER_MEDIA_ANDROID)
    add_subdirectory(android)
    list(APPEND media_flutter_libs media_flutter_android)
endif ()

if (MEDIA_USE_SDL)
    list(APPEND media_flutter_libs media_sdl)
endif ()

add_subdirectory(base)

list(APPEND media_flutter_sources
        ffp_flutter.cc
        ffp_flutter.h
        media_api.h
        third_party/dart/dart_api_dl.c
        video_renderer_sink_impl.cc
        video_renderer_sink_impl.h
        )

if (MEDIA_BUILD_STATIC)
    add_library(media_flutter STATIC ${media_flutter_sources})
else ()
    add_library(media_flutter SHARED ${media_flutter_sources})
endif ()

target_include_directories(media_flutter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} third_party)
target_link_libraries(media_flutter PUBLIC media_flutter_base ${media_flutter_libs})

if (MEDIA_BUILD_STATIC)
    install(TARGETS media_base media_flutter ${media_flutter_libs} media_flutter_base
            ARCHIVE DESTINATION lib)
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/media_api.h
            DESTINATION include)
endif ()