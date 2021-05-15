cmake_minimum_required(VERSION 3.10)

project("media_flutter")

aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR} MEDIA_FLUTTER_SOURCES)

add_library(media_flutter SHARED ${MEDIA_FLUTTER_SOURCES} third_party/dart/dart_api_dl.c)

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
    install(TARGETS ${PROJECT_NAME}
            LIBRARY DESTINATION lib
            PUBLIC_HEADER DESTINATION include
            )

    install(DIRECTORY "${PROJECT_SOURCE_DIR}/"
            DESTINATION include
            FILES_MATCHING PATTERN "ffp_flutter.h")
endif ()

#if (APPLE)
#    set_target_properties(media_flutter PROPERTIES
#            FRAMEWORK TRUE
#            MACOSX_FRAMEWORK_IDENTIFIER tech.soit.media_flutter
#            PUBLIC_HEADER ffp_flutter.h
#            )
#endif ()