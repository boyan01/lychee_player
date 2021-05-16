cmake_minimum_required(VERSION 3.10)

project("media_flutter")

add_library(media_flutter SHARED
        ffp_flutter.cc
        ffp_flutter.h
        media_api.h
        third_party/dart/dart_api_dl.c)

target_include_directories(media_flutter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} third_party)

list(APPEND media_flutter_libs media_player)

if (CMAKE_FLUTTER_MEDIA_WINDOWS)
    add_subdirectory(windows)
    list(APPEND media_flutter_libs media_flutter_windows)
elseif (CMAKE_FLUTTER_MEDIA_LINUX)
    list(APPEND media_flutter_libs flutter)
endif ()

if (CMAKE_FLUTTER_MEDIA_ANDROID)
    add_subdirectory(android)
    list(APPEND media_flutter_libs media_flutter_android)
endif ()

if (MEDIA_USE_SDL)
    list(APPEND media_flutter_libs media_sdl)
endif ()

add_subdirectory(base)

target_link_libraries(media_flutter PUBLIC media_flutter_base ${media_flutter_libs})

if (APPLE)
    set_target_properties(media_flutter PROPERTIES
            FRAMEWORK TRUE
            MACOSX_FRAMEWORK_IDENTIFIER tech.soit.flutter.media
            PUBLIC_HEADER "media_api.h"
            )

    install(TARGETS ${PROJECT_NAME}
            LIBRARY DESTINATION lib
            FRAMEWORK DESTINATION frameworks
            PUBLIC_HEADER DESTINATION include
            )
endif ()