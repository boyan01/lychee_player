cmake_minimum_required(VERSION 3.10)

project("media_flutter")

list(APPEND media_flutter_libs media_player)

if (MEDIA_WINDOWS)
    list(APPEND media_flutter_libs)
elseif (MEDIA_LINUX)
    list(APPEND media_flutter_libs flutter)
elseif (MEDIA_MACOS OR MEDIA_IOS)
    list(APPEND media_flutter_libs media_macos)
    set(MEDIA_BUILD_STATIC 1)
endif ()

if (MEDIA_ANDROID)
    add_subdirectory(android)
    list(APPEND media_flutter_libs media_flutter_android)
endif ()

if (MEDIA_USE_SDL)
    list(APPEND media_flutter_libs media_sdl)
endif ()

list(APPEND media_flutter_sources
        ffp_flutter.cc
        ffp_flutter.h
        third_party/dart/dart_api_dl.c
        )

if (MEDIA_BUILD_STATIC)
    add_library(media_flutter STATIC ${media_flutter_sources})
else ()
    add_library(media_flutter SHARED ${media_flutter_sources})
endif ()

target_include_directories(media_flutter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} third_party)
target_link_libraries(media_flutter PUBLIC ${media_flutter_libs})

if (MEDIA_BUILD_STATIC)
    install(TARGETS media_base media_flutter ${media_flutter_libs} ARCHIVE DESTINATION lib)
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/ffp_flutter.h
            DESTINATION include)
endif ()